// Standalone check of the ASTC weight-range field R, Table C.2.6 / C.2.7.
//
// Cross-validates two competing readings of the R field against basisu's
// shipped UASTC mode table, which pairs each ASTC block mode with the weight
// range the encoder actually wrote. Nothing here shares panDXVK's decoder.

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

struct Mode {
  int row = -1;          // C.2.7 table row
  int gridW = 0, gridH = 0;
  int H = -1, D = -1;
  int R2 = -1, R1 = -1, R0 = -1;
  int rangeA = -1;       // reading under test
  int rangeB = -1;       // reference reading
  bool legal = false;
  const char* why = "";
};

static Mode decode(uint32_t v, int order) {
  Mode m;
  m.D = (v >> 10) & 1;
  m.H = (v >> 9) & 1;
  m.R2 = (v >> 1) & 1;
  m.R1 = v & 1;
  m.R0 = (v >> 4) & 1;

  // Rows 6-10, void-extent and reserved all carry bits[1:0] == 00.
  if (m.R2 == 0 && m.R1 == 0) { m.why = "bits[1:0]==00"; return m; }
  if (((v >> 6) & 7u) == 7u)  { m.why = "reserved (bits[8:6]==111)"; return m; }
  if ((v & 0x1FFu) == 0x1FCu) { m.why = "void-extent"; return m; }
  if (m.R0 == 0 && ((v >> 2) & 3u) != 0)
    { m.why = "reserved (R0==0 with bits[3:2]!=00)"; return m; }

  // order 0 = panDXVK's historical reading, order 1 = reference reading.
  m.rangeA = (m.R0 << 2) | (m.R2 << 1) | m.R1;   // R0 as MSB  (legacy)
  m.rangeB = (m.R2 << 2) | (m.R1 << 1) | m.R0;   // R2 as MSB  (reference)

  const int R = order ? m.rangeB : m.rangeA;
  const int bits32 = (v >> 2) & 3;
  const int a = (v >> 5) & 3;

  switch (bits32) {
    case 0: m.gridW = ((v >> 7) & 3) + 4; m.gridH = a + 2; m.row = 1; break;
    case 1: m.gridW = ((v >> 7) & 3) + 8; m.gridH = a + 2; m.row = 2; break;
    case 2: m.gridW = a + 2; m.gridH = ((v >> 7) & 3) + 8; m.row = 3; break;
    default:
      if (((v >> 8) & 1) == 0) { m.gridW = a + 2; m.gridH = ((v >> 7) & 1) + 6; m.row = 4; }
      else                     { m.gridW = ((v >> 7) & 1) + 2; m.gridH = a + 2; m.row = 5; }
      break;
  }

  // Table C.2.6: R = 000 and R = 001 are Invalid for both H = 0 and H = 1.
  if (R <= 1) { m.why = "invalid R"; return m; }
  m.legal = true;
  return m;
}

// Table C.2.6 -> (bits, trits, quints, maxValue). -1 = invalid.
static void rangeSpec(int R, int H, int& bits, int& trits, int& quints, int& max) {
  bits = trits = quints = max = -1;
  // Table C.2.6, spec lines 681-688. Both 000 and 001 are Invalid for both H.
  if (H) {
    static const int T[8][4] = {
      {-1,-1,-1,-1}, {-1,-1,-1,-1},
      {1,0,1,9}, {2,1,0,11}, {4,0,0,15}, {2,0,1,19}, {3,1,0,23}, {5,0,0,31}
    };
    bits = T[R][0]; trits = T[R][1]; quints = T[R][2]; max = T[R][3];
  } else {
    static const int T[8][4] = {
      {-1,-1,-1,-1}, {-1,-1,-1,-1},
      {1,0,0,1}, {0,1,0,2}, {2,0,0,3}, {0,0,1,4}, {1,1,0,5}, {3,0,0,7}
    };
    bits = T[R][0]; trits = T[R][1]; quints = T[R][2]; max = T[R][3];
  }
}

// spec C.2.22 weight_bits formula
static int weightBits(int n, int bits, int trits, int quints) {
  int a = 0;
  if (trits)  a += (n * 8 * trits + 4) / 5;
  if (quints) a += (n * 7 * quints + 2) / 3;
  return a + n * bits;
}

int main() {
  int fail = 0;

  // ---- basisu basisu_transcoder.cpp:11887 / :11277 (mode index -> block, range idx)
  struct Pair { uint32_t mode; int idx; const char* name; };
  static const Pair k[] = {
    {0x42,   2, "uastc mode 1"},  {0x53,   5, "uastc mode 2"},
    {0x442,  2, "uastc mode 6"},  {0x242,  8, "uastc mode 0"},
    {0x441,  0, "uastc mode 13"}, {0x253, 11, "uastc mode 18"},
  };
  // basisu g_astc_bise_range_table -> (bits, trits, quints)
  struct Idx { int b, t, q, max; };
  static const Idx tbl[] = {
    {1,0,0,1},{0,1,0,2},{2,0,0,3},{0,0,1,4},{1,1,0,5},{3,0,0,7},
    {1,0,1,9},{2,1,0,11},{4,0,0,15},{3,0,1,19},{2,1,0,23},{5,0,0,31}
  };

  printf("== cross-check against basisu shipped UASTC table ==\n");
  int okA = 0, okB = 0;
  for (auto& p : k) {
    Mode a = decode(p.mode, 0), b = decode(p.mode, 1);
    int ba, ta, qa, ma, bb, tb, qb, mb;
    rangeSpec(a.rangeA, a.H, ba, ta, qa, ma);
    rangeSpec(b.rangeB, b.H, bb, tb, qb, mb);
    const Idx& e = tbl[p.idx];
    bool matchA = a.legal && ba == e.b && ta == e.t && qa == e.q;
    bool matchB = b.legal && bb == e.b && tb == e.t && qb == e.q;
    okA += matchA; okB += matchB;
    printf("  %-14s 0x%-5x legacy->R=%d H=%d w%dt%dq%d %-4s | ref->R=%d H=%d w%dt%dq%d %s\n",
           p.name, p.mode, a.rangeA, a.H, ba, ta, qa, matchA ? "OK" : "NO",
           b.rangeB, b.H, bb, tb, qb, matchB ? "MATCH" : "MISMATCH");
  }
  printf("  legacy reading %d/6 correct, reference reading %d/6 correct\n\n", okA, okB);
  if (okB != 6) { printf("FAIL: reference reading does not reproduce basisu\n"); fail++; }

  // ---- what 0x242 and 0x241 actually mean
  printf("== candidate block modes, 4x4 grid ==\n");
  for (uint32_t v : {0x51u, 0x52u, 0x241u, 0x242u}) {
    Mode m = decode(v, 1);
    int b, t, q, mx;
    rangeSpec(m.rangeB, m.H, b, t, q, mx);
    int wb = (m.legal && m.gridW == 4 && m.gridH == 4) ? weightBits(16, b, t, q) : 0;
    printf("  0x%-5x row=%d %dx%d H=%d R=%d range=%d..%d (w%dt%dq%d) weight_bits=%d",
           v, m.row, m.gridW, m.gridH, m.H, m.rangeB, 0, mx, b, t, q, wb);
    if (m.legal && m.gridW == 4 && m.gridH == 4) {
      int rem = 128 - 17 - wb;
      printf("  remaining=%d", rem);
      if (v == 0x241u || v == 0x242u) printf("  <- v6/v7 candidate");
    }
    printf("%s\n", m.legal ? "" : (std::string(" ILLEGAL(") + m.why + ")").c_str());
  }
  printf("\n");

  // ---- uniqueness: exactly one non-dual-plane mode gives 4x4 + quint+1bit (0..9)
  int n = 0; uint32_t found = 0; uint32_t dual = 0;
  for (uint32_t v = 0; v < 2048; v++) {
    Mode m = decode(v, 1);
    if (!m.legal || m.gridW != 4 || m.gridH != 4) continue;
    int b, t, q, mx;
    rangeSpec(m.rangeB, m.H, b, t, q, mx);
    if (b == 1 && q == 1) { (m.D ? (dual = v, 0) : (n++, found = v)); }
  }
  printf("non-dual-plane 4x4 + 1 quint + 1 bit (range 0..9): %d", n);
  if (n == 1) printf("  -> 0x%x", found);
  printf("   (dual-plane sibling: 0x%x)\n", dual);
  if (n != 1 || found != 0x241u) { printf("FAIL: expected unique non-dual-plane 0x241\n"); fail++; }

  // ---- v6's shipped constant, for the record
  printf("\n== v6 constant ==\n");
  {
    Mode m = decode(0x51u, 1);
    int b, t, q, mx;
    rangeSpec(m.rangeB, m.H, b, t, q, mx);
    printf("  0x51 as shipped   -> range 0..%d (%d quints / %d trits), weight_bits=%d\n",
           mx, q, t, weightBits(16, b, t, q));
    printf("  v6 intended 0..4  -> block mode 0x52 (bit0/bit1 swapped)\n");
  }

  // ---- v7 layout fit under the corrected reading
  printf("\n== v7 layout fit ==\n");
  {
    int wb = weightBits(16, 1, 0, 1);           // 0..9
    int rem = 128 - 17 - wb;
    // largest ISE range whose 8-value CEM size fits `rem`
    struct Rg { int v, b, t, q; };
    static const Rg rg[] = {{255,8,0,0},{191,6,1,0},{159,5,0,1},{127,7,0,0},
                            {95,5,1,0},{79,4,0,1},{63,6,0,0},{47,4,1,0},
                            {39,3,0,1},{31,5,0,0},{23,3,1,0},{19,2,0,1},
                            {15,4,0,0},{11,2,1,0},{9,1,0,1},{7,3,0,0},
                            {5,1,1,0},{4,0,0,1},{3,2,0,0},{2,0,1,0},{1,1,0,0}};
    printf("  weight_bits=%d remaining=%d\n", wb, rem);
    for (auto& r : rg) {
      int nvals = 8;
      int sz = 0;
      if (r.t) sz += (nvals * 8 * r.t + 4) / 5;
      if (r.q) sz += (nvals * 7 * r.q + 2) / 3;
      sz += nvals * r.b;
      if (sz <= rem) {
        printf("  selected endpoint range 0..%d  size=%d <= %d  bit-only=%d\n",
               r.v, sz, rem, (r.t == 0 && r.q == 0));
        if (r.v != 127 || sz != 56) { printf("FAIL: expected 0..127 / 56 bits\n"); fail++; }
        break;
      }
    }
  }

  printf("\n%s\n", fail ? "RMCHK: FAIL" : "RMCHK: ALL PASS");
  return fail;
}
