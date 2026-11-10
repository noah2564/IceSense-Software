// Host test: feeds algorithm/testvectors.csv (generated from the Python reference) through the
// C++ port and compares every output. Build and run with tests/test_cpp_equivalence.py or:
//   g++ -std=c++17 -I../../firmware/lib/icedetect icedetect_test.cpp ../../firmware/lib/icedetect/icedetect.cpp -o t
//   ./t ../testvectors.csv
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "icedetect.h"

using icedetect::Detector;
using icedetect::Input;
using icedetect::Params;

static void applyOverrides(Params& p, const char* text) {
  // "# params k=v k=v  [title]" -> overrides
  std::string s(text);
  size_t end = s.find('[');
  if (end != std::string::npos) s = s.substr(0, end);
  size_t pos = 0;
  while ((pos = s.find('=', pos)) != std::string::npos) {
    size_t ks = s.rfind(' ', pos);
    ks = ks == std::string::npos ? 0 : ks + 1;
    size_t ve = s.find(' ', pos);
    std::string key = s.substr(ks, pos - ks);
    double v = atof(s.substr(pos + 1, ve == std::string::npos ? std::string::npos : ve - pos - 1).c_str());
    if (key == "freeze_c") p.freeze_c = v;
    else if (key == "deposit_pct") p.deposit_pct = v;
    else if (key == "ice_pct") p.ice_pct = v;
    else if (key == "liquid_log10") p.liquid_log10 = v;
    else if (key == "mm_per_pct") p.mm_per_pct = v;
    else if (key == "max_mm") p.max_mm = v;
    else if (key == "confirm_windows") p.confirm_windows = (int)v;
    else if (key == "defrost_on_mm") p.defrost_on_mm = v;
    else if (key == "defrost_on_windows") p.defrost_on_windows = (int)v;
    else if (key == "defrost_off_mm") p.defrost_off_mm = v;
    else if (key == "defrost_off_windows") p.defrost_off_windows = (int)v;
    else { fprintf(stderr, "unknown param '%s'\n", key.c_str()); exit(2); }
    pos++;
  }
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s testvectors.csv\n", argv[0]); return 2; }
  FILE* fh = fopen(argv[1], "r");
  if (!fh) { perror("open"); return 2; }
  Detector det;
  char line[512];
  long rows = 0, bad = 0, sections = 0;
  while (fgets(line, sizeof line, fh)) {
    if (line[0] == '\n' || line[0] == '\r') continue;
    if (line[0] == '#') {
      if (strncmp(line, "# params", 8) == 0) {
        Params p;
        applyOverrides(p, line + 8);
        det.setParams(p);
        det.reset();
        sections++;
      }
      continue;
    }
    double cap, r, t, thick;
    int state, defrost;
    if (sscanf(line, "%lf,%lf,%lf,%d,%lf,%d", &cap, &r, &t, &state, &thick, &defrost) != 6) {
      fprintf(stderr, "bad row: %s", line);
      return 2;
    }
    Input in;
    in.cap_delta_pct = cap;
    in.res_log10 = r;
    in.temp_c = t;
    auto out = det.step(in);
    rows++;
    bool ok = (int)out.state == state && std::fabs(out.thickness_mm - thick) < 1e-9 &&
              out.defrost_request == (defrost != 0);
    if (!ok) {
      if (++bad <= 10)
        fprintf(stderr, "MISMATCH row %ld: got state=%d thick=%.9g defrost=%d, expected %d %.9g %d\n", rows,
                (int)out.state, out.thickness_mm, (int)out.defrost_request, state, thick, defrost);
    }
  }
  fclose(fh);
  printf("%ld rows in %ld sections, %ld mismatches\n", rows, sections, bad);
  return bad ? 1 : 0;
}
