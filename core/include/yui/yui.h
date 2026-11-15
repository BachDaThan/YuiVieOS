/* libyui — the stable C ABI of YuiVieOS (Tầng 4).
 *
 * Apps (Flutter/Yui, GTK, games, scripts) link libyui.so or libyui.a and get the
 * same brain yuid exposes over its socket, without parsing JSON-RPC.
 *
 * Contract:
 *   - every function is thread-safe unless documented otherwise;
 *   - handles returned by *_new must be released with the matching *_free;
 *   - buffers returned through yui_buf own their memory; call yui_buf_free;
 *   - strings returned by yui_version/yui_arch are static, do not free;
 *   - integer return codes are negative on failure (see YUI_E_*);
 *   - functions that fill a caller buffer return the number of bytes written
 *     (so 0 means "empty result", which is still success) — plus NUL for strings.
 */
#ifndef YUI_H
#define YUI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YUI_ABI_VERSION 1

enum {
  YUI_OK = 0,
  YUI_E_INVALID = -1,
  YUI_E_NOMEM = -2,
  YUI_E_AUTH = -3,
  YUI_E_CORRUPT = -4,
  YUI_E_UNAVAILABLE = -5,
  YUI_E_TOO_SMALL = -6
};

typedef struct yui_buf {
  uint8_t* data;
  size_t len;
} yui_buf;

/* Build/identity */
const char* yui_version(void);
const char* yui_arch(void);
int yui_abi_version(void);

/* "avx2" | "neon" | "rvv" | "scalar" */
int yui_simd(char* out, size_t cap);

/* SHA-256 of the input as 64 lowercase hex chars + NUL. */
int yui_sha256(const void* data, size_t len, char out_hex[65]);

/* YUIP container: compress/decompress with integrity checks. */
int yui_pack(const void* data, size_t len, int level, yui_buf* out);
int yui_unpack(const void* data, size_t len, yui_buf* out, char* err, size_t err_cap);
void yui_buf_free(yui_buf* buf);

/* Elastic k/W autotuning (Tầng 2 policy, computed by Tầng 4). */
typedef struct yui_tune {
  int k;
  int window;
  int threads;
  char profile[32]; /* may-yeu | can-bang | may-manh */
  char reason[256]; /* Vietnamese explanation */
} yui_tune;

int yui_tune_auto(int ram_decigb, int temperature_c, int on_battery, int threads, int has_gpu,
                  yui_tune* out);

/* Knowledge graph handle. */
typedef struct yui_graph yui_graph;
yui_graph* yui_graph_new(void);
void yui_graph_free(yui_graph* g);
int yui_graph_upsert(yui_graph* g, const char* id, const char* label, const char* kind);
int yui_graph_link(yui_graph* g, const char* from, const char* rel, const char* to, double weight);
/* Returns a malloc'd JSON document; free with yui_free(). */
char* yui_graph_dump(yui_graph* g);
int yui_graph_import(yui_graph* g, const char* json_doc);
size_t yui_graph_nodes(const yui_graph* g);
/* Fills `out` with the ids reachable from `start` up to `max_depth`, newline
 * separated.  Returns the number written, or a negative YUI_E_* code. */
int yui_graph_bfs(yui_graph* g, const char* start, int max_depth, char* out, size_t cap);

/* Vietnamese input methods. */
int yui_telex(const char* keys, char* out, size_t cap);
int yui_vni(const char* keys, char* out, size_t cap);

/* Vault */
int yui_vault_seal(const char* password, const void* plain, size_t len, int log2n, yui_buf* out);
int yui_vault_open(const char* password, const void* blob, size_t len, yui_buf* out, char* err,
                   size_t err_cap);

/* Sandbox policy evaluation. Returns 1 allowed, 0 denied, negative on error. */
int yui_sandbox_check(const char* policy_json, const char* kind, const char* target, char* reason,
                      size_t reason_cap);

void yui_free(void* ptr);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YUI_H */
