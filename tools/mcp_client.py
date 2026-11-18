#!/usr/bin/env python3
"""YuiVieOS MCP Client — Real integration client for Model Context Protocol (P1.8).

Connects to yuid --mcp over stdio, performs MCP handshake, lists available tools,
and exercises graph_upsert -> graph_link -> graph_query.
"""

import argparse
import json
import os
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional


class McpClient:
    """Client implementing the Model Context Protocol stdio transport."""

    def __init__(self, command: List[str], verbose: bool = False):
        self.command = command
        self.verbose = verbose
        self.proc: Optional[subprocess.Popen] = None
        self._next_id = 1

    def start(self) -> None:
        """Start the MCP server subprocess."""
        if self.verbose:
            print(f"[mcp] Khởi động tiến trình máy chủ: {' '.join(self.command)}")
        self.proc = subprocess.Popen(
            self.command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def close(self) -> None:
        """Terminate the server process if still running."""
        if self.proc:
            if self.proc.poll() is None:
                try:
                    self.send_request("shutdown", {})
                except Exception:
                    pass
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
            self.proc = None

    def _send_raw(self, payload: bytes) -> None:
        assert self.proc and self.proc.stdin
        header = f"Content-Length: {len(payload)}\r\n\r\n".encode("utf-8")
        self.proc.stdin.write(header + payload)
        self.proc.stdin.flush()

    def _read_raw(self) -> Optional[bytes]:
        assert self.proc and self.proc.stdout
        line = self.proc.stdout.readline()
        if not line:
            return None
        length = 0
        while line.strip():
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":")[1].strip())
            line = self.proc.stdout.readline()
        if length == 0:
            return None
        return self.proc.stdout.read(length)

    def send_request(self, method: str, params: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        """Send a JSON-RPC request and wait for the response."""
        req_id = self._next_id
        self._next_id += 1
        req = {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": method,
            "params": params if params is not None else {},
        }
        raw = json.dumps(req).encode("utf-8")
        if self.verbose:
            print(f"[mcp ->] {method} (id={req_id})")
        self._send_raw(raw)
        resp_raw = self._read_raw()
        if not resp_raw:
            raise RuntimeError(f"Máy chủ MCP đóng kết nối khi gọi {method}")
        resp = json.loads(resp_raw.decode("utf-8"))
        if self.verbose:
            print(f"[mcp <-] Nhận phản hồi id={resp.get('id')}: {resp}")
        if "error" in resp:
            err = resp["error"]
            raise RuntimeError(f"Lỗi MCP ({err.get('code')}): {err.get('message')}")
        return resp.get("result", {})

    def initialize(self) -> Dict[str, Any]:
        """Perform MCP initialize handshake."""
        return self.send_request(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {
                    "name": "yui-external-agent",
                    "version": "1.0.0",
                },
            },
        )

    def list_tools(self) -> List[Dict[str, Any]]:
        """List all tools advertised by the MCP server."""
        res = self.send_request("tools/list", {})
        return res.get("tools", [])

    def call_tool(self, name: str, arguments: Dict[str, Any]) -> Any:
        """Call a specific tool via tools/call and parse the text content."""
        res = self.send_request("tools/call", {"name": name, "arguments": arguments})
        is_error = res.get("isError", False)
        content_items = res.get("content", [])
        combined_text = "".join(item.get("text", "") for item in content_items if item.get("type") == "text")
        if is_error:
            raise RuntimeError(f"Công cụ '{name}' trả về lỗi: {combined_text}")
        try:
            return json.loads(combined_text)
        except Exception:
            return combined_text


def run_integration_test(server_cmd: List[str], verbose: bool = False) -> bool:
    """Run full integration flow: initialize -> tools/list -> graph_upsert -> graph_link -> graph_query."""
    print("=" * 70)
    print("YuiVieOS — Bắt đầu kiểm thử tích hợp MCP (Task P1.8)")
    print(f"Lệnh máy chủ: {' '.join(server_cmd)}")
    print("=" * 70)

    client = McpClient(server_cmd, verbose=verbose)
    try:
        client.start()

        # 1. Bắt tay initialize
        print("\n[1/5] Bắt tay khởi tạo giao thức MCP (initialize)...")
        init_res = client.initialize()
        server_info = init_res.get("serverInfo", {})
        print(f"  ✓ Máy chủ: {server_info.get('name')} v{server_info.get('version')}")
        print(f"  ✓ Phiên bản giao thức: {init_res.get('protocolVersion')}")
        print(f"  ✓ Hướng dẫn hệ thống: {init_res.get('instructions')}")

        # 2. Liệt kê công cụ (tools/list)
        print("\n[2/5] Lấy danh sách công cụ được hỗ trợ (tools/list)...")
        tools = client.list_tools()
        tool_names = [t["name"] for t in tools]
        print(f"  ✓ Tìm thấy {len(tools)} công cụ MCP: {', '.join(tool_names)}")
        assert "graph_upsert" in tool_names, "Thiếu công cụ graph_upsert"
        assert "graph_query" in tool_names, "Thiếu công cụ graph_query"
        assert "graph_link" in tool_names, "Thiếu công cụ graph_link"

        # 3. Chèn nút đồ thị (graph_upsert)
        print("\n[3/5] Tác tử ngoài gọi công cụ graph_upsert...")
        node1_id = "agent_doc_42"
        node1_label = "Kế hoạch An ninh Hệ thống YuiVieOS"
        res_up1 = client.call_tool(
            "graph_upsert",
            {
                "id": node1_id,
                "label": node1_label,
                "kind": "doc",
                "props": {"author": "Security-Agent", "tier": "L1-L3", "created": int(time.time())},
            },
        )
        print(f"  ✓ Đã chèn nút 1: id='{node1_id}', kết quả={res_up1}")
        assert res_up1.get("id") == node1_id, f"ID không khớp: {res_up1}"

        node2_id = "agent_note_99"
        node2_label = "Nhật ký kiểm toán O(1) Capability Revocation"
        res_up2 = client.call_tool(
            "graph_upsert",
            {
                "id": node2_id,
                "label": node2_label,
                "kind": "note",
                "props": {"status": "verified"},
            },
        )
        print(f"  ✓ Đã chèn nút 2: id='{node2_id}', kết quả={res_up2}")

        # 4. Nối liên kết quan hệ (graph_link)
        print("\n[4/5] Tác tử ngoài tạo liên kết giữa hai nút (graph_link)...")
        res_link = client.call_tool(
            "graph_link",
            {
                "from": node1_id,
                "rel": "references",
                "to": node2_id,
                "weight": 1.0,
            },
        )
        print(f"  ✓ Đã liên kết: '{node1_id}' --[references]--> '{node2_id}', kết quả={res_link}")

        # 5. Truy vấn đồ thị (graph_query)
        print("\n[5/5] Tác tử ngoài gọi graph_query để xác minh dữ liệu đồ thị...")
        res_q = client.call_tool(
            "graph_query",
            {
                "q": node1_id,
                "expand_from": node1_id,
                "depth": 2,
            },
        )
        hits = res_q.get("hits", [])
        expanded = res_q.get("expanded", [])
        print(f"  ✓ Kết quả tìm kiếm (hits): {len(hits)} nút")
        for hit in hits:
            print(f"    - Nút id='{hit.get('id')}', nhãn='{hit.get('label')}', phân loại='{hit.get('kind')}'")
        print(f"  ✓ Danh sách nút mở rộng BFS (expanded): {expanded}")

        hit_ids = [h.get("id") for h in hits]
        assert node1_id in hit_ids, f"Nút '{node1_id}' không có trong kết quả tìm kiếm!"
        assert node2_id in expanded, f"Nút liên kết '{node2_id}' không có trong kết quả BFS!"

        print("\n" + "=" * 70)
        print("✓ BẰNG CHỨNG HOÀN TẤT TASK P1.8:")
        print(f"  Tác tử ngoài đã gọi thành công graph_upsert -> graph_link -> graph_query.")
        print(f"  Dữ liệu đồ thị tri thức được bảo toàn và truy vấn chuẩn xác qua MCP.")
        print("=" * 70)
        return True
    finally:
        client.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="YuiVieOS MCP External Client")
    parser.add_argument(
        "--server",
        default="./build/bin/yuid",
        help="Đường dẫn file thực thi yuid (mặc định: ./build/bin/yuid)",
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="In chi tiết gói tin JSON-RPC trên giao thức MCP",
    )
    args = parser.parse_args()

    server_path = os.path.abspath(args.server)
    if not os.path.exists(server_path):
        print(f"Lỗi: Không tìm thấy máy chủ yuid tại '{server_path}'", file=sys.stderr)
        return 1

    success = run_integration_test([server_path, "--mcp"], verbose=args.verbose)
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
