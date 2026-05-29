/* tiny harness: read a .swift file, run msf_analyze, print error count.
   Used to reproduce/debug frontend hangs natively (run under `sample`). */
#include <msf.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: analyze_one FILE\n"); return 2; }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror("open"); return 2; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char *buf = malloc(n + 1);
  size_t rd = fread(buf, 1, n, f); buf[rd] = '\0'; fclose(f);
  fprintf(stderr, "analyze begin (%ld bytes)\n", n);
  MSFResult *r = msf_analyze(buf, argv[1]);
  fprintf(stderr, "analyze done\n");
  if (r) { printf("errors=%u\n", msf_error_count(r)); msf_result_free(r); }
  free(buf);
  return 0;
}
