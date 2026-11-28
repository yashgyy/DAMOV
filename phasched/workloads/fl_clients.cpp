// fl_clients.cpp - synthetic FedAvg clients for the PhaSched evaluation.
//
// One FL round = INGRESS (NIC writes the global model into DRAM, fl_dma_in)
//              -> TRAIN (K local SGD minibatches on synthetic local data)
//              -> EGRESS (NIC reads the updated model out of DRAM, fl_dma_out).
// The model parameters live in ONE contiguous buffer W (what the NIC transfers);
// dense models keep gradients in a second buffer G of the same layout.
//
// Models (model=...):
//   logreg : sparse multinomial logistic regression, hogwild SGD   (feat x classes params)
//   nb     : multinomial naive Bayes, count accumulation            (feat x classes params)
//   dnn    : MLP in-h1-h2-classes, ReLU, softmax-CE, minibatch SGD
//   lstm   : 2-layer LSTM (input in, hidden H, seq T) + linear classifier, full BPTT
//   gru    : 2-layer GRU, same shapes as lstm
//   cnn    : conv3x3(3->c1)-pool-conv3x3(c1->c2)-pool-FC(fc)-FC(classes) on img x img x 3 images
//
// Usage: fl_client model=dnn rounds=1 steps=4 batch=32 threads=4 [size keys] [check=1]
//   size keys: feat nnz classes in h1 h2 hidden seq img c1 c2 fc data_mb lr
//   check=1  : native sanity check - repeats one minibatch and prints the loss (must drop)
//
// Build: g++ -O3 -std=c++11 -fopenmp -I../include -I../../simulator/misc/hooks fl_clients.cpp -o fl_client
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <chrono>
#include <omp.h>
#include "phase_hint.h"

// ------------------------------------------------------------------ arguments
struct Args {
    std::string model = "dnn";
    int rounds = 1, steps = 4, batch = 0, threads = 4, check = 0;
    long feat = 1000000, nnz = 64, classes = 10;
    long in = 1024, h1 = 2048, h2 = 2048;     // dnn (in is also the rnn input width if set)
    long hidden = 512, seq = 16;              // rnn
    long img = 32, c1 = 64, c2 = 128, fc = 1024;  // cnn
    long data_mb = 256;
    float lr = 0.01f;
    bool in_set = false;
};

static Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string s(argv[i]);
        size_t e = s.find('=');
        if (e == std::string::npos) { fprintf(stderr, "bad arg %s (use key=value)\n", argv[i]); exit(1); }
        std::string k = s.substr(0, e), v = s.substr(e + 1);
        if      (k == "model")   a.model = v;
        else if (k == "rounds")  a.rounds = atoi(v.c_str());
        else if (k == "steps")   a.steps = atoi(v.c_str());
        else if (k == "batch")   a.batch = atoi(v.c_str());
        else if (k == "threads") a.threads = atoi(v.c_str());
        else if (k == "check")   a.check = atoi(v.c_str());
        else if (k == "feat")    a.feat = atol(v.c_str());
        else if (k == "nnz")     a.nnz = atol(v.c_str());
        else if (k == "classes") a.classes = atol(v.c_str());
        else if (k == "in")      { a.in = atol(v.c_str()); a.in_set = true; }
        else if (k == "h1")      a.h1 = atol(v.c_str());
        else if (k == "h2")      a.h2 = atol(v.c_str());
        else if (k == "hidden")  a.hidden = atol(v.c_str());
        else if (k == "seq")     a.seq = atol(v.c_str());
        else if (k == "img")     a.img = atol(v.c_str());
        else if (k == "c1")      a.c1 = atol(v.c_str());
        else if (k == "c2")      a.c2 = atol(v.c_str());
        else if (k == "fc")      a.fc = atol(v.c_str());
        else if (k == "data_mb") a.data_mb = atol(v.c_str());
        else if (k == "lr")      a.lr = (float)atof(v.c_str());
        else { fprintf(stderr, "unknown key %s\n", k.c_str()); exit(1); }
    }
    return a;
}

// ------------------------------------------------------------------ helpers
static inline uint64_t xs64(uint64_t& s) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static inline float frand(uint64_t& s) { return (float)((xs64(s) >> 40) * (1.0 / 16777216.0)) * 2.f - 1.f; }

static float* falloc(size_t n) {
    void* p = nullptr;
    if (posix_memalign(&p, 4096, n * sizeof(float) + 64)) { fprintf(stderr, "alloc failed\n"); exit(1); }
    return (float*)p;
}

static void fill_rand(float* x, size_t n, float scale, uint64_t seed) {
    #pragma omp parallel
    {
        int t = omp_get_thread_num(), T = omp_get_num_threads();
        size_t a = n * t / T, b = n * (t + 1) / T;
        uint64_t s = seed * 2654435761ULL + t * 0x9E3779B97F4A7C15ULL + 1;
        for (size_t i = a; i < b; i++) x[i] = scale * frand(s);
    }
}

static void zero(float* x, size_t n) {
    #pragma omp parallel
    {
        int t = omp_get_thread_num(), T = omp_get_num_threads();
        size_t a = n * t / T, b = n * (t + 1) / T;
        memset(x + a, 0, (b - a) * sizeof(float));
    }
}

static void sgd(float* W, const float* G, size_t n, float lr) {
    #pragma omp parallel for schedule(static)
    for (long i = 0; i < (long)n; i++) W[i] -= lr * G[i];
}

static inline float sigm(float x) { return 1.f / (1.f + expf(-x)); }

// Y[B x n] (+)= X[B x m] * W^T + b      (W is n x m, row-major)
static void lin_fwd(const float* X, int B, int m, const float* W, const float* b, int n, float* Y, bool acc) {
    #pragma omp parallel for schedule(static)
    for (int o = 0; o < n; o++) {
        const float* w = W + (size_t)o * m;
        for (int i = 0; i < B; i++) {
            const float* x = X + (size_t)i * m;
            float s = b ? b[o] : 0.f;
            for (int k = 0; k < m; k++) s += x[k] * w[k];
            float* y = Y + (size_t)i * n + o;
            *y = acc ? *y + s : s;
        }
    }
}

// dX[B x m] (+)= dY[B x n] * W        (W is n x m); each thread owns a column slice
static void lin_bwd_data(const float* dY, int B, int n, const float* W, int m, float* dX, bool acc) {
    #pragma omp parallel
    {
        int t = omp_get_thread_num(), T = omp_get_num_threads();
        int k0 = (int)((long)m * t / T), k1 = (int)((long)m * (t + 1) / T);
        if (!acc) for (int i = 0; i < B; i++) memset(dX + (size_t)i * m + k0, 0, (k1 - k0) * sizeof(float));
        for (int o = 0; o < n; o++) {
            const float* w = W + (size_t)o * m;
            for (int i = 0; i < B; i++) {
                float d = dY[(size_t)i * n + o];
                if (d == 0.f) continue;
                float* dx = dX + (size_t)i * m;
                for (int k = k0; k < k1; k++) dx[k] += d * w[k];
            }
        }
    }
}

// G[n x m] += dY^T[n x B] * X[B x m],  gb[n] += sum_i dY[i]
static void lin_bwd_w(const float* dY, int B, int n, const float* X, int m, float* G, float* gb) {
    #pragma omp parallel for schedule(static)
    for (int o = 0; o < n; o++) {
        float* g = G + (size_t)o * m;
        float sb = 0.f;
        for (int i = 0; i < B; i++) {
            float d = dY[(size_t)i * n + o];
            sb += d;
            if (d == 0.f) continue;
            const float* x = X + (size_t)i * m;
            for (int k = 0; k < m; k++) g[k] += d * x[k];
        }
        if (gb) gb[o] += sb;
    }
}

// softmax cross-entropy: returns mean loss, writes dL/dlogits (already / B)
static double softmax_ce(const float* L, const int* y, int B, int C, float* dL) {
    double loss = 0;
    for (int i = 0; i < B; i++) {
        const float* l = L + (size_t)i * C;
        float mx = l[0];
        for (int c = 1; c < C; c++) mx = l[c] > mx ? l[c] : mx;
        double z = 0;
        for (int c = 0; c < C; c++) z += exp((double)(l[c] - mx));
        for (int c = 0; c < C; c++) {
            double p = exp((double)(l[c] - mx)) / z;
            dL[(size_t)i * C + c] = (float)((p - (c == y[i] ? 1.0 : 0.0)) / B);
        }
        loss += -(l[y[i]] - mx - log(z));
    }
    return loss / B;
}

// ------------------------------------------------------------------ model base
struct Model {
    size_t P = 0;            // number of parameters (floats) in W
    float* W = nullptr;      // model parameters (NIC transfers this buffer)
    float* G = nullptr;      // gradients (dense models)
    size_t data_bytes = 0;
    virtual ~Model() {}
    virtual void init(const Args& a) = 0;
    virtual double step(long k) = 0;   // one local minibatch, returns loss
};

// ------------------------------------------------------------------ sparse models
struct Sparse : Model {
    int C, NNZ, B; long V, N; float lr;
    uint32_t* idx = nullptr; float* val = nullptr; int* lab = nullptr;
    void init_data(const Args& a, int defB) {
        C = (int)a.classes; NNZ = (int)a.nnz; V = a.feat; lr = a.lr; B = a.batch ? a.batch : defB;
        N = (a.data_mb << 20) / (NNZ * 8L);
        if (N < B) N = B;
        idx = (uint32_t*)malloc(sizeof(uint32_t) * N * NNZ);
        val = falloc((size_t)N * NNZ);
        lab = (int*)malloc(sizeof(int) * N);
        #pragma omp parallel
        {
            int t = omp_get_thread_num(), T = omp_get_num_threads();
            uint64_t s = 12345 + 977 * t;
            for (long n = N * t / T; n < N * (t + 1) / T; n++) {
                for (int j = 0; j < NNZ; j++) { idx[n * NNZ + j] = (uint32_t)(xs64(s) % V); val[n * NNZ + j] = 1.f + 0.5f * frand(s); }
                lab[n] = (int)(xs64(s) % C);
            }
        }
        data_bytes = (size_t)N * NNZ * 8 + N * 4;
        P = (size_t)V * C + C;
        W = falloc(P);
    }
};

// logistic regression: hogwild SGD, per-sample sparse row updates (read-modify-write of W rows)
struct LogReg : Sparse {
    void init(const Args& a) { init_data(a, 512); fill_rand(W, P, 0.01f, 7); }
    double step(long k) {
        long s0 = (k * B) % (N - B + 1);
        double loss = 0;
        #pragma omp parallel for schedule(static) reduction(+:loss)
        for (int b = 0; b < B; b++) {
            long n = s0 + b;
            const uint32_t* ix = idx + n * NNZ; const float* vx = val + n * NNZ;
            float lg[64]; int Cc = C < 64 ? C : 64;
            for (int c = 0; c < Cc; c++) lg[c] = W[(size_t)V * C + c];
            for (int j = 0; j < NNZ; j++) { const float* r = W + (size_t)ix[j] * C; for (int c = 0; c < Cc; c++) lg[c] += vx[j] * r[c]; }
            float mx = lg[0]; for (int c = 1; c < Cc; c++) mx = lg[c] > mx ? lg[c] : mx;
            float z = 0; for (int c = 0; c < Cc; c++) { lg[c] = expf(lg[c] - mx); z += lg[c]; }
            for (int c = 0; c < Cc; c++) lg[c] = lg[c] / z - (c == lab[n] ? 1.f : 0.f);
            loss += -logf(lg[lab[n]] + 1.f + 1e-12f);
            for (int j = 0; j < NNZ; j++) { float* r = W + (size_t)ix[j] * C; float g = lr * vx[j]; for (int c = 0; c < Cc; c++) r[c] -= g * lg[c]; }
            for (int c = 0; c < Cc; c++) W[(size_t)V * C + c] -= lr * lg[c];
        }
        return loss / B;
    }
};

// naive Bayes: count accumulation (read-modify-write scattered over the count table)
struct NaiveBayes : Sparse {
    void init(const Args& a) { init_data(a, 512); fill_rand(W, P, 0.f, 7); }
    double step(long k) {
        long s0 = (k * B) % (N - B + 1);
        #pragma omp parallel for schedule(static)
        for (int b = 0; b < B; b++) {
            long n = s0 + b; int y = lab[n];
            const uint32_t* ix = idx + n * NNZ; const float* vx = val + n * NNZ;
            for (int j = 0; j < NNZ; j++) W[(size_t)ix[j] * C + y] += vx[j];
            W[(size_t)V * C + y] += 1.f;
        }
        return 0.0;
    }
};

// ------------------------------------------------------------------ DNN (MLP)
struct DNN : Model {
    int B, I, H1, H2, C; long N; float lr;
    float *X, *W1, *b1, *W2, *b2, *W3, *b3, *G1, *gb1, *G2, *gb2, *G3, *gb3;
    float *A1, *A2, *O, *dO, *dA2, *dA1; int* lab;
    void init(const Args& a) {
        B = a.batch ? a.batch : 32; I = (int)a.in; H1 = (int)a.h1; H2 = (int)a.h2; C = (int)a.classes; lr = a.lr;
        N = (a.data_mb << 20) / (I * 4L); if (N < B) N = B;
        X = falloc((size_t)N * I); fill_rand(X, (size_t)N * I, 1.f, 3);
        lab = (int*)malloc(sizeof(int) * N); uint64_t s = 99; for (long n = 0; n < N; n++) lab[n] = (int)(xs64(s) % C);
        data_bytes = (size_t)N * I * 4;
        size_t o1 = 0, ob1 = o1 + (size_t)H1 * I, o2 = ob1 + H1, ob2 = o2 + (size_t)H2 * H1, o3 = ob2 + H2, ob3 = o3 + (size_t)C * H2;
        P = ob3 + C; W = falloc(P); G = falloc(P);
        fill_rand(W, P, 0.03f, 5); zero(G, P);
        W1 = W + o1; b1 = W + ob1; W2 = W + o2; b2 = W + ob2; W3 = W + o3; b3 = W + ob3;
        G1 = G + o1; gb1 = G + ob1; G2 = G + o2; gb2 = G + ob2; G3 = G + o3; gb3 = G + ob3;
        A1 = falloc((size_t)B * H1); A2 = falloc((size_t)B * H2); O = falloc((size_t)B * C); dO = falloc((size_t)B * C);
        dA2 = falloc((size_t)B * H2); dA1 = falloc((size_t)B * H1);
    }
    double step(long k) {
        long s0 = (k * B) % (N - B + 1);
        const float* x = X + (size_t)s0 * I;
        zero(G, P);
        lin_fwd(x, B, I, W1, b1, H1, A1, false);  for (long j = 0; j < (long)B * H1; j++) A1[j] = A1[j] > 0 ? A1[j] : 0;
        lin_fwd(A1, B, H1, W2, b2, H2, A2, false); for (long j = 0; j < (long)B * H2; j++) A2[j] = A2[j] > 0 ? A2[j] : 0;
        lin_fwd(A2, B, H2, W3, b3, C, O, false);
        double loss = softmax_ce(O, lab + s0, B, C, dO);
        lin_bwd_w(dO, B, C, A2, H2, G3, gb3);
        lin_bwd_data(dO, B, C, W3, H2, dA2, false); for (long j = 0; j < (long)B * H2; j++) if (A2[j] <= 0) dA2[j] = 0;
        lin_bwd_w(dA2, B, H2, A1, H1, G2, gb2);
        lin_bwd_data(dA2, B, H2, W2, H1, dA1, false); for (long j = 0; j < (long)B * H1; j++) if (A1[j] <= 0) dA1[j] = 0;
        lin_bwd_w(dA1, B, H1, x, I, G1, gb1);
        sgd(W, G, P, lr);
        return loss;
    }
};

// ------------------------------------------------------------------ RNN (LSTM / GRU), 2 layers, full BPTT
struct RNN : Model {
    bool lstm; int gc;               // gates: 4 (LSTM: i f g o) or 3 (GRU: r z n)
    int B, I, H, T, C, L = 2; long N; float lr;
    float* X; int* lab;
    struct Layer { int in; float *Wx, *bx, *Wh, *bh, *GWx, *gbx, *GWh, *gbh;
                   float *xin, *act, *gh, *c, *h, *dx; } ly[2];
    float *Wo, *bo, *GWo, *gbo, *O, *dO, *dh, *dc, *dpre, *dgh, *tmp;
    explicit RNN(bool is_lstm) : lstm(is_lstm), gc(is_lstm ? 4 : 3) {}
    void init(const Args& a) {
        B = a.batch ? a.batch : 8; I = (int)(a.in_set ? a.in : 256); H = (int)a.hidden; T = (int)a.seq; C = (int)a.classes; lr = a.lr;
        N = (a.data_mb << 20) / ((long)T * I * 4); if (N < B) N = B;
        X = falloc((size_t)N * T * I); fill_rand(X, (size_t)N * T * I, 1.f, 11);
        lab = (int*)malloc(sizeof(int) * N); uint64_t s = 77; for (long n = 0; n < N; n++) lab[n] = (int)(xs64(s) % C);
        data_bytes = (size_t)N * T * I * 4;
        size_t G4 = (size_t)gc * H, off = 0, offs[2][4];
        for (int l = 0; l < L; l++) {
            int in = l == 0 ? I : H; ly[l].in = in;
            offs[l][0] = off; off += G4 * in; offs[l][1] = off; off += G4;
            offs[l][2] = off; off += G4 * H;  offs[l][3] = off; off += G4;
        }
        size_t oWo = off; off += (size_t)C * H; size_t obo = off; off += C;
        P = off; W = falloc(P); G = falloc(P); fill_rand(W, P, 0.05f, 13); zero(G, P);
        for (int l = 0; l < L; l++) {
            Layer& y = ly[l];
            y.Wx = W + offs[l][0]; y.bx = W + offs[l][1]; y.Wh = W + offs[l][2]; y.bh = W + offs[l][3];
            y.GWx = G + offs[l][0]; y.gbx = G + offs[l][1]; y.GWh = G + offs[l][2]; y.gbh = G + offs[l][3];
            y.xin = falloc((size_t)T * B * y.in);
            y.act = falloc((size_t)T * B * G4);
            y.gh  = falloc((size_t)T * B * G4);
            y.c   = falloc((size_t)(T + 1) * B * H);
            y.h   = falloc((size_t)(T + 1) * B * H);
            y.dx  = falloc((size_t)T * B * H);         // dL/dh coming from the layer above (layer 0 only)
        }
        Wo = W + oWo; bo = W + obo; GWo = G + oWo; gbo = G + obo;
        O = falloc((size_t)B * C); dO = falloc((size_t)B * C);
        dh = falloc((size_t)B * H); dc = falloc((size_t)B * H); tmp = falloc((size_t)B * H);
        dpre = falloc((size_t)B * G4); dgh = falloc((size_t)B * G4);
    }
    void fwd_layer(Layer& y) {
        size_t G4 = (size_t)gc * H;
        memset(y.h, 0, sizeof(float) * B * H); memset(y.c, 0, sizeof(float) * B * H);
        for (int t = 0; t < T; t++) {
            float* xt = y.xin + (size_t)t * B * y.in;
            float* hp = y.h + (size_t)t * B * H; float* hn = y.h + (size_t)(t + 1) * B * H;
            float* a = y.act + (size_t)t * B * G4; float* gh = y.gh + (size_t)t * B * G4;
            lin_fwd(xt, B, y.in, y.Wx, y.bx, (int)G4, a, false);
            lin_fwd(hp, B, H, y.Wh, y.bh, (int)G4, gh, false);
            if (lstm) {
                float* cp = y.c + (size_t)t * B * H; float* cn = y.c + (size_t)(t + 1) * B * H;
                for (int i = 0; i < B; i++) for (int j = 0; j < H; j++) {
                    float* r = a + (size_t)i * G4; const float* q = gh + (size_t)i * G4;
                    float ig = sigm(r[j] + q[j]), fg = sigm(r[H + j] + q[H + j]);
                    float gg = tanhf(r[2 * H + j] + q[2 * H + j]), og = sigm(r[3 * H + j] + q[3 * H + j]);
                    r[j] = ig; r[H + j] = fg; r[2 * H + j] = gg; r[3 * H + j] = og;
                    float c = fg * cp[(size_t)i * H + j] + ig * gg;
                    cn[(size_t)i * H + j] = c; hn[(size_t)i * H + j] = og * tanhf(c);
                }
            } else {
                for (int i = 0; i < B; i++) for (int j = 0; j < H; j++) {
                    float* r = a + (size_t)i * G4; const float* q = gh + (size_t)i * G4;
                    float rg = sigm(r[j] + q[j]), zg = sigm(r[H + j] + q[H + j]);
                    float ng = tanhf(r[2 * H + j] + rg * q[2 * H + j]);
                    r[j] = rg; r[H + j] = zg; r[2 * H + j] = ng;
                    hn[(size_t)i * H + j] = (1.f - zg) * ng + zg * hp[(size_t)i * H + j];
                }
            }
        }
    }
    // dtop: per-timestep dL/dh from above (T x B x H) or nullptr; dlast: dL/dh_T (B x H) or nullptr
    // below: layer whose dx (dL/d its h_t) receives this layer's input gradient, or nullptr
    void bwd_layer(Layer& y, const float* dtop, const float* dlast, Layer* below) {
        size_t G4 = (size_t)gc * H;
        memset(dh, 0, sizeof(float) * B * H); memset(dc, 0, sizeof(float) * B * H);
        if (dlast) memcpy(dh, dlast, sizeof(float) * B * H);
        for (int t = T - 1; t >= 0; t--) {
            if (dtop) { const float* d = dtop + (size_t)t * B * H; for (long j = 0; j < (long)B * H; j++) dh[j] += d[j]; }
            float* xt = y.xin + (size_t)t * B * y.in;
            float* hp = y.h + (size_t)t * B * H;
            float* a = y.act + (size_t)t * B * G4; float* gh = y.gh + (size_t)t * B * G4;
            if (lstm) {
                float* cp = y.c + (size_t)t * B * H; float* cn = y.c + (size_t)(t + 1) * B * H;
                for (int i = 0; i < B; i++) for (int j = 0; j < H; j++) {
                    const float* r = a + (size_t)i * G4; size_t ij = (size_t)i * H + j;
                    float ig = r[j], fg = r[H + j], gg = r[2 * H + j], og = r[3 * H + j];
                    float tc = tanhf(cn[ij]);
                    float dcc = dc[ij] + dh[ij] * og * (1.f - tc * tc);
                    float* d = dpre + (size_t)i * G4;
                    d[j] = dcc * gg * ig * (1.f - ig);
                    d[H + j] = dcc * cp[ij] * fg * (1.f - fg);
                    d[2 * H + j] = dcc * ig * (1.f - gg * gg);
                    d[3 * H + j] = dh[ij] * tc * og * (1.f - og);
                    dc[ij] = dcc * fg;
                }
                lin_bwd_w(dpre, B, (int)G4, xt, y.in, y.GWx, y.gbx);
                lin_bwd_w(dpre, B, (int)G4, hp, H, y.GWh, y.gbh);
                if (below) lin_bwd_data(dpre, B, (int)G4, y.Wx, y.in, below->dx + (size_t)t * B * H, false);
                lin_bwd_data(dpre, B, (int)G4, y.Wh, H, dh, false);
            } else {
                for (int i = 0; i < B; i++) for (int j = 0; j < H; j++) {
                    const float* r = a + (size_t)i * G4; const float* q = gh + (size_t)i * G4; size_t ij = (size_t)i * H + j;
                    float rg = r[j], zg = r[H + j], ng = r[2 * H + j];
                    float dz = dh[ij] * (hp[ij] - ng), dn = dh[ij] * (1.f - zg);
                    tmp[ij] = dh[ij] * zg;                               // direct path to h_{t-1}
                    float dnp = dn * (1.f - ng * ng);
                    float drp = dnp * q[2 * H + j] * rg * (1.f - rg);
                    float dzp = dz * zg * (1.f - zg);
                    float* dx_ = dpre + (size_t)i * G4; float* dg = dgh + (size_t)i * G4;
                    dx_[j] = drp; dx_[H + j] = dzp; dx_[2 * H + j] = dnp;
                    dg[j]  = drp; dg[H + j]  = dzp; dg[2 * H + j]  = dnp * rg;
                }
                lin_bwd_w(dpre, B, (int)G4, xt, y.in, y.GWx, y.gbx);
                lin_bwd_w(dgh, B, (int)G4, hp, H, y.GWh, y.gbh);
                if (below) lin_bwd_data(dpre, B, (int)G4, y.Wx, y.in, below->dx + (size_t)t * B * H, false);
                lin_bwd_data(dgh, B, (int)G4, y.Wh, H, dh, false);
                for (long j = 0; j < (long)B * H; j++) dh[j] += tmp[j];
            }
        }
    }
    double step(long k) {
        long s0 = (k * B) % (N - B + 1);
        zero(G, P);
        // gather the minibatch into time-major layout: xin[t][b][:]
        for (int t = 0; t < T; t++) for (int b = 0; b < B; b++)
            memcpy(ly[0].xin + ((size_t)t * B + b) * I, X + ((size_t)(s0 + b) * T + t) * I, sizeof(float) * I);
        fwd_layer(ly[0]);
        for (int t = 0; t < T; t++) memcpy(ly[1].xin + (size_t)t * B * H, ly[0].h + (size_t)(t + 1) * B * H, sizeof(float) * B * H);
        fwd_layer(ly[1]);
        float* hT = ly[1].h + (size_t)T * B * H;
        lin_fwd(hT, B, H, Wo, bo, C, O, false);
        double loss = softmax_ce(O, lab + s0, B, C, dO);
        lin_bwd_w(dO, B, C, hT, H, GWo, gbo);
        float* dlast = falloc((size_t)B * H);
        lin_bwd_data(dO, B, C, Wo, H, dlast, false);
        bwd_layer(ly[1], nullptr, dlast, &ly[0]);
        bwd_layer(ly[0], ly[0].dx, nullptr, nullptr);
        free(dlast);
        sgd(W, G, P, lr);
        return loss;
    }
};

// ------------------------------------------------------------------ CNN
// conv3x3(3->c1)+ReLU+maxpool2 -> conv3x3(c1->c2)+ReLU+maxpool2 -> FC(fc)+ReLU -> FC(classes)
// NHWC layout; convolution = im2col + the dense kernels above (rows = B*H*W).
struct CNN : Model {
    int B, S, C1, C2, F, C; long N; float lr;
    float *X; int *lab;
    float *Wc1, *bc1, *Wc2, *bc2, *Wf1, *bf1, *Wf2, *bf2, *Gc1, *gbc1, *Gc2, *gbc2, *Gf1, *gbf1, *Gf2, *gbf2;
    float *col1, *A1, *P1, *col2, *A2, *P2, *H, *O, *dO, *dH, *dP2, *dA2, *dcol2, *dP1, *dA1;
    int *arg1, *arg2;
    static void im2col(const float* in, int B, int S, int Cin, float* col) {
        #pragma omp parallel for schedule(static)
        for (long r = 0; r < (long)B * S * S; r++) {
            int b = (int)(r / (S * S)), y = (int)(r / S % S), x = (int)(r % S);
            float* c = col + r * 9 * Cin;
            for (int ky = 0; ky < 3; ky++) for (int kx = 0; kx < 3; kx++) {
                int yy = y + ky - 1, xx = x + kx - 1;
                float* d = c + (ky * 3 + kx) * Cin;
                if (yy < 0 || yy >= S || xx < 0 || xx >= S) memset(d, 0, sizeof(float) * Cin);
                else memcpy(d, in + (((size_t)b * S + yy) * S + xx) * Cin, sizeof(float) * Cin);
            }
        }
    }
    static void col2im(const float* col, int B, int S, int Cin, float* in) {   // in = sum of col contributions
        memset(in, 0, sizeof(float) * (size_t)B * S * S * Cin);
        for (long r = 0; r < (long)B * S * S; r++) {
            int b = (int)(r / (S * S)), y = (int)(r / S % S), x = (int)(r % S);
            const float* c = col + r * 9 * Cin;
            for (int ky = 0; ky < 3; ky++) for (int kx = 0; kx < 3; kx++) {
                int yy = y + ky - 1, xx = x + kx - 1;
                if (yy < 0 || yy >= S || xx < 0 || xx >= S) continue;
                float* d = in + (((size_t)b * S + yy) * S + xx) * Cin; const float* s = c + (ky * 3 + kx) * Cin;
                for (int k = 0; k < Cin; k++) d[k] += s[k];
            }
        }
    }
    static void relu(float* a, long n) { for (long i = 0; i < n; i++) a[i] = a[i] > 0 ? a[i] : 0; }
    static void pool(const float* in, int B, int S, int Cc, float* out, int* arg) {   // 2x2 max, NHWC
        int T = S / 2;
        #pragma omp parallel for schedule(static)
        for (long o = 0; o < (long)B * T * T; o++) {
            int b = (int)(o / (T * T)), y = (int)(o / T % T), x = (int)(o % T);
            for (int c = 0; c < Cc; c++) {
                float best = -1e30f; int bi = 0;
                for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
                    int idx = (int)((((size_t)b * S + 2 * y + dy) * S + 2 * x + dx) * Cc + c);
                    if (in[idx] > best) { best = in[idx]; bi = idx; }
                }
                out[o * Cc + c] = best; arg[o * Cc + c] = bi;
            }
        }
    }
    void init(const Args& a) {
        B = a.batch ? a.batch : 16; S = (int)a.img; C1 = (int)a.c1; C2 = (int)a.c2; F = (int)a.fc; C = (int)a.classes; lr = a.lr;
        long in = (long)S * S * 3; N = (a.data_mb << 20) / (in * 4); if (N < B) N = B;
        X = falloc((size_t)N * in); fill_rand(X, (size_t)N * in, 1.f, 21);
        lab = (int*)malloc(sizeof(int) * N); uint64_t s = 55; for (long n = 0; n < N; n++) lab[n] = (int)(xs64(s) % C);
        data_bytes = (size_t)N * in * 4;
        long flat = (long)(S / 4) * (S / 4) * C2;
        size_t o[9]; o[0] = 0; o[1] = o[0] + (size_t)C1 * 27; o[2] = o[1] + C1; o[3] = o[2] + (size_t)C2 * 9 * C1; o[4] = o[3] + C2;
        o[5] = o[4] + (size_t)F * flat; o[6] = o[5] + F; o[7] = o[6] + (size_t)C * F; o[8] = o[7] + C;
        P = o[8]; W = falloc(P); G = falloc(P); fill_rand(W, P, 0.05f, 23); zero(G, P);
        Wc1 = W + o[0]; bc1 = W + o[1]; Wc2 = W + o[2]; bc2 = W + o[3]; Wf1 = W + o[4]; bf1 = W + o[5]; Wf2 = W + o[6]; bf2 = W + o[7];
        Gc1 = G + o[0]; gbc1 = G + o[1]; Gc2 = G + o[2]; gbc2 = G + o[3]; Gf1 = G + o[4]; gbf1 = G + o[5]; Gf2 = G + o[6]; gbf2 = G + o[7];
        long r1 = (long)B * S * S, r2 = (long)B * (S / 2) * (S / 2);
        col1 = falloc((size_t)r1 * 27); A1 = falloc((size_t)r1 * C1); P1 = falloc((size_t)r2 * C1); arg1 = (int*)malloc(sizeof(int) * r2 * C1);
        col2 = falloc((size_t)r2 * 9 * C1); A2 = falloc((size_t)r2 * C2); P2 = falloc((size_t)B * flat); arg2 = (int*)malloc(sizeof(int) * B * flat);
        H = falloc((size_t)B * F); O = falloc((size_t)B * C); dO = falloc((size_t)B * C); dH = falloc((size_t)B * F);
        dP2 = falloc((size_t)B * flat); dA2 = falloc((size_t)r2 * C2); dcol2 = falloc((size_t)r2 * 9 * C1); dP1 = falloc((size_t)r2 * C1);
        dA1 = falloc((size_t)r1 * C1);
    }
    double step(long k) {
        long s0 = (k * B) % (N - B + 1);
        const float* x = X + (size_t)s0 * S * S * 3;
        int S2 = S / 2; long r1 = (long)B * S * S, r2 = (long)B * S2 * S2, flat = (long)(S / 4) * (S / 4) * C2;
        zero(G, P);
        im2col(x, B, S, 3, col1);  lin_fwd(col1, (int)r1, 27, Wc1, bc1, C1, A1, false); relu(A1, r1 * C1); pool(A1, B, S, C1, P1, arg1);
        im2col(P1, B, S2, C1, col2); lin_fwd(col2, (int)r2, 9 * C1, Wc2, bc2, C2, A2, false); relu(A2, r2 * C2); pool(A2, B, S2, C2, P2, arg2);
        lin_fwd(P2, B, (int)flat, Wf1, bf1, F, H, false); relu(H, (long)B * F);
        lin_fwd(H, B, F, Wf2, bf2, C, O, false);
        double loss = softmax_ce(O, lab + s0, B, C, dO);
        lin_bwd_w(dO, B, C, H, F, Gf2, gbf2);
        lin_bwd_data(dO, B, C, Wf2, F, dH, false); for (long j = 0; j < (long)B * F; j++) if (H[j] <= 0) dH[j] = 0;
        lin_bwd_w(dH, B, F, P2, (int)flat, Gf1, gbf1);
        lin_bwd_data(dH, B, F, Wf1, (int)flat, dP2, false);
        memset(dA2, 0, sizeof(float) * r2 * C2);
        for (long j = 0; j < (long)B * flat; j++) dA2[arg2[j]] += dP2[j];
        for (long j = 0; j < r2 * C2; j++) if (A2[j] <= 0) dA2[j] = 0;
        lin_bwd_w(dA2, (int)r2, C2, col2, 9 * C1, Gc2, gbc2);
        lin_bwd_data(dA2, (int)r2, C2, Wc2, 9 * C1, dcol2, false);
        col2im(dcol2, B, S2, C1, dP1);
        memset(dA1, 0, sizeof(float) * r1 * C1);
        for (long j = 0; j < r2 * C1; j++) dA1[arg1[j]] += dP1[j];
        for (long j = 0; j < r1 * C1; j++) if (A1[j] <= 0) dA1[j] = 0;
        lin_bwd_w(dA1, (int)r1, C1, col1, 27, Gc1, gbc1);
        sgd(W, G, P, lr);
        return loss;
    }
};

// ------------------------------------------------------------------ main
int main(int argc, char** argv) {
    Args a = parse(argc, argv);
    omp_set_num_threads(a.threads);
    Model* m = nullptr;
    if      (a.model == "logreg") m = new LogReg();
    else if (a.model == "nb")     m = new NaiveBayes();
    else if (a.model == "dnn")    m = new DNN();
    else if (a.model == "lstm")   m = new RNN(true);
    else if (a.model == "gru")    m = new RNN(false);
    else if (a.model == "cnn")    m = new CNN();
    else { fprintf(stderr, "unknown model %s\n", a.model.c_str()); return 1; }
    m->init(a);
    printf("[FL] model=%s params=%zu model_MB=%.1f grad_MB=%.1f data_MB=%.1f rounds=%d steps=%d threads=%d\n",
           a.model.c_str(), m->P, m->P * 4.0 / (1 << 20), m->G ? m->P * 4.0 / (1 << 20) : 0.0,
           m->data_bytes / 1048576.0, a.rounds, a.steps, a.threads);
    fflush(stdout);

    if (a.check == 2 && m->G) {   // numeric gradient check (run with lr=0 threads=1)
        m->step(0);
        uint64_t s = 4242;
        for (int q = 0; q < 12; q++) {
            size_t p = (size_t)(xs64(s) % m->P);
            m->step(0);                                 // gradient at the unperturbed point
            float ga = m->G[p], w = m->W[p], e = 1e-3f;
            m->W[p] = w + e; double l1 = m->step(0);
            m->W[p] = w - e; double l2 = m->step(0);
            m->W[p] = w;
            printf("[FL] gradcheck p=%zu analytic=% .6e numeric=% .6e\n", p, ga, (l1 - l2) / (2 * e));
        }
        return 0;
    }
    if (a.check) {   // native sanity check: the loss on one repeated minibatch must go down
        for (int i = 0; i < 8; i++) printf("[FL] check iter %d loss %.5f\n", i, m->step(0));
        return 0;
    }

    typedef std::chrono::steady_clock clk;
    std::vector<double> tin(a.rounds), ttr(a.rounds), teg(a.rounds); double last = 0;
    zsim_roi_begin();
    for (int r = 0; r < a.rounds; r++) {
        clk::time_point t0 = clk::now();
        fl_phase(FL_INGRESS);
        fl_dma_in(m->W, m->P * sizeof(float));             // NIC writes the global model
        clk::time_point t1 = clk::now();
        fl_phase(FL_TRAIN);
        for (int k = 0; k < a.steps; k++) last = m->step((long)r * a.steps + k);
        clk::time_point t2 = clk::now();
        fl_phase(FL_EGRESS);
        fl_dma_out(m->W, m->P * sizeof(float));            // NIC reads the update
        fl_phase(FL_IDLE);
        clk::time_point t3 = clk::now();
        tin[r] = std::chrono::duration<double, std::milli>(t1 - t0).count();
        ttr[r] = std::chrono::duration<double, std::milli>(t2 - t1).count();
        teg[r] = std::chrono::duration<double, std::milli>(t3 - t2).count();
    }
    zsim_roi_end();
    for (int r = 0; r < a.rounds; r++)
        printf("[FL] round %d ingress_ms=%.2f train_ms=%.2f egress_ms=%.2f\n", r, tin[r], ttr[r], teg[r]);
    printf("[FL] last_loss=%.5f checksum=%.6f\n", last, (double)m->W[m->P / 2]);
    return 0;
}
