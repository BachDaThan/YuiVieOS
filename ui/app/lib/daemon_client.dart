// DaemonClient — nói chuyện với yuid qua Unix socket.
//
// Giao thức: một đối tượng JSON-RPC 2.0 trên mỗi dòng (xem docs/PROTOCOL.md).
// Không có trạng thái phiên: mỗi lời gọi tự chứa `id`, daemon trả đúng `id` đó.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

class DaemonException implements Exception {
  DaemonException(this.message, {this.code});
  final String message;
  final int? code;

  @override
  String toString() => code == null ? message : '[$code] $message';
}

class DaemonClient {
  DaemonClient({this.socketPath = '/run/yuid.sock'});

  final String socketPath;
  Socket? _socket;
  StreamIterator<String>? _lines;
  int _nextId = 1;

  bool get isConnected => _socket != null;

  Future<void> connect() async {
    await close();
    final socket = await Socket.connect(
      InternetAddress(socketPath, type: InternetAddressType.unix),
      0,
      timeout: const Duration(seconds: 3),
    );
    _socket = socket;
    _lines = StreamIterator<String>(
      socket
          .cast<List<int>>()
          .transform(utf8.decoder)
          .transform(const LineSplitter())
          .where((line) => line.trim().isNotEmpty),
    );
  }

  Future<void> close() async {
    await _lines?.cancel();
    _lines = null;
    _socket?.destroy();
    _socket = null;
  }

  /// Gọi một phương thức và trả về `result` (ném DaemonException nếu lỗi).
  Future<Map<String, dynamic>> call(
    String method, [
    Map<String, dynamic> params = const {},
  ]) async {
    final socket = _socket;
    final lines = _lines;
    if (socket == null || lines == null) {
      throw DaemonException('Chưa kết nối tới yuid ($socketPath)');
    }
    final id = _nextId++;
    final request = jsonEncode({
      'jsonrpc': '2.0',
      'id': id,
      'method': method,
      'params': params,
    });
    socket.write('$request\n');
    await socket.flush();

    if (!await lines.moveNext().timeout(const Duration(seconds: 10))) {
      throw DaemonException('yuid đóng kết nối trước khi trả lời');
    }
    final decoded = jsonDecode(lines.current);
    if (decoded is! Map<String, dynamic>) {
      throw DaemonException('Trả lời không phải đối tượng JSON');
    }
    final error = decoded['error'];
    if (error is Map<String, dynamic>) {
      throw DaemonException(
        (error['message'] as String?) ?? 'lỗi không rõ',
        code: error['code'] as int?,
      );
    }
    final result = decoded['result'];
    return result is Map<String, dynamic> ? result : <String, dynamic>{};
  }

  // ----------------------------------------------------------- tiện ích --

  Future<String> convertKeys(String keys, {String method = 'telex'}) async {
    final r = await call('ime.convert', {'text': keys, 'method': method});
    return (r['text'] as String?) ?? keys;
  }

  Future<String> version() async {
    final r = await call('yuid.version');
    return (r['version'] as String?) ?? '?';
  }

  Future<List<Map<String, dynamic>>> search(String q) async {
    final r = await call('graph.query', {'q': q});
    final hits = r['hits'];
    return hits is List ? hits.cast<Map<String, dynamic>>() : const [];
  }

  Future<void> remember(String id, String label, {String kind = 'concept'}) async {
    await call('graph.upsert', {'id': id, 'label': label, 'kind': kind});
  }

  Future<String> seal(String password, String plain) async {
    final r = await call('vault.seal', {
      'plain': base64Encode(utf8.encode(plain)),
      'key': password,
    });
    return (r['blob'] as String?) ?? '';
  }
}
