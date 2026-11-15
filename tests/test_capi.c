/* C11 consumer of libyui — proves the ABI is usable from plain C, not just C++.
 * Build: cc -std=gnu11 -Iinclude test_capi.c libyui.a -lstdc++ -lpthread
 */
#include <stdio.h>
#include <string.h>

#include "yui/yui.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++checks;                                                           \
    if (cond) {                                                         \
      printf("  ok     %s\n", #cond);                                   \
    } else {                                                            \
      ++failures;                                                       \
      printf("  FAIL   %s (%s:%d)\n", #cond, __FILE__, __LINE__);       \
    }                                                                   \
  } while (0)

int main(void) {
  char out[256];
  char hex[65];

  CHECK(yui_abi_version() == YUI_ABI_VERSION);
  CHECK(strlen(yui_version()) > 0);
  CHECK(strlen(yui_arch()) > 0);
  CHECK(yui_simd(out, sizeof(out)) > 0);
  CHECK(strlen(out) > 0);

  CHECK(yui_sha256("abc", 3, hex) == YUI_OK);
  CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

  /* Telex/VNI from C. */
  CHECK(yui_telex("Tieengs Vieetj", out, sizeof(out)) > 0);
  CHECK(strcmp(out, "Tiếng Việt") == 0);
  CHECK(yui_vni("Vie65t Nam", out, sizeof(out)) > 0);
  CHECK(strcmp(out, "Việt Nam") == 0);
  CHECK(yui_telex("ab", out, 2) == YUI_E_TOO_SMALL);  /* needs 2 chars + NUL */

  /* Compression round-trip. */
  {
    static char text[8192];
    size_t i, len = 0;
    yui_buf packed = {0, 0}, plain = {0, 0};
    char err[128] = {0};
    for (i = 0; i < 60; ++i)
      len += (size_t)snprintf(text + len, sizeof(text) - len, "YuiVieOS nen du lieu bang YUI-LZ. ");
    CHECK(yui_pack(text, len, 7, &packed) == YUI_OK);
    CHECK(packed.len > 0 && packed.len < len / 4);
    CHECK(yui_unpack(packed.data, packed.len, &plain, err, sizeof(err)) == YUI_OK);
    CHECK(plain.len == len);
    CHECK(memcmp(plain.data, text, plain.len) == 0);
    yui_buf_free(&packed);
    yui_buf_free(&plain);
  }

  /* Tuning. */
  {
    yui_tune tune;
    memset(&tune, 0, sizeof(tune));
    CHECK(yui_tune_auto(320, 45, 0, 16, 1, &tune) == YUI_OK);
    CHECK(strcmp(tune.profile, "may-manh") == 0);
    CHECK(tune.k >= 8);
  }

  /* Graph. */
  {
    yui_graph *g = yui_graph_new();
    char *doc = NULL;
    char ids[128];
    CHECK(g != NULL);
    CHECK(yui_graph_upsert(g, "n1", "Nut mot", "note") == YUI_OK);
    CHECK(yui_graph_upsert(g, "n2", "Nut hai", "note") == YUI_OK);
    CHECK(yui_graph_link(g, "n1", "next", "n2", 1.0) == YUI_OK);
    CHECK(yui_graph_nodes(g) == 2);
    doc = yui_graph_dump(g);
    CHECK(doc != NULL && strstr(doc, "Nut mot") != NULL);
    yui_graph_import(g, "{\"format\":\"yui-graph/1\",\"nodes\":[{\"id\":\"x\"}],\"edges\":[]}");
    CHECK(yui_graph_nodes(g) == 1);
    yui_graph_upsert(g, "y", "Nut y", "note");
    CHECK(yui_graph_bfs(g, "x", 1, ids, sizeof(ids)) >= 0);
    yui_free(doc);
    yui_graph_free(g);
  }

  /* Vault. */
  {
    yui_buf sealed = {0, 0}, opened = {0, 0};
    char err[128] = {0};
    CHECK(yui_vault_seal("khoa", "bi mat", 6, 10, &sealed) == YUI_OK);
    CHECK(sealed.len > 74);
    CHECK(yui_vault_open("khoa", sealed.data, sealed.len, &opened, err, sizeof(err)) == YUI_OK);
    CHECK(opened.len == 6 && memcmp(opened.data, "bi mat", 6) == 0);
    yui_buf_free(&opened);
    CHECK(yui_vault_open("sai", sealed.data, sealed.len, &opened, err, sizeof(err)) == YUI_E_AUTH);
    yui_buf_free(&sealed);
  }

  /* Sandbox policy from C. */
  {
    char reason[256];
    const char *policy = "{\"app\":\"c-app\",\"tier\":\"native\",\"capabilities\":[\"fs.read\",\"fs:/etc:ro\"]}";
    CHECK(yui_sandbox_check(policy, "fs.read", "/etc/yui/x", reason, sizeof(reason)) == 1);
    CHECK(yui_sandbox_check(policy, "fs.write", "/etc/yui/x", reason, sizeof(reason)) == 0);
    CHECK(strlen(reason) > 0);
  }

  printf("\n%s: %d checks, %d failed\n", failures ? "FAILED" : "OK", checks, failures);
  return failures ? 1 : 0;
}
