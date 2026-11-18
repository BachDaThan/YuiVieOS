#include <stdio.h>
#include <string.h>
#include <yui/yui.h>

int main(void) {
  printf("YuiVieOS C Demo\n");
  printf("  Phiên bản: %s\n", yui_version());
  printf("  Kiến trúc: %s\n", yui_arch());

  char telex_out[128];
  int rc1 = yui_ime_convert("telex", "Tieengs Vieetj", telex_out, sizeof(telex_out));
  if (rc1 <= 0 || strcmp(telex_out, "Tiếng Việt") != 0) {
    fprintf(stderr, "Lỗi Telex: %s\n", telex_out);
    return 1;
  }
  printf("  Telex: 'Tieengs Vieetj' -> '%s'\n", telex_out);

  char vni_out[128];
  int rc2 = yui_ime_convert("vni", "Vie65t Nam", vni_out, sizeof(vni_out));
  if (rc2 <= 0 || strcmp(vni_out, "Việt Nam") != 0) {
    fprintf(stderr, "Lỗi VNI: %s\n", vni_out);
    return 1;
  }
  printf("  VNI  : 'Vie65t Nam' -> '%s'\n", vni_out);

  printf("OK: C demo thành công\n");
  return 0;
}
