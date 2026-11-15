// App Yui — giao diện Tầng 5 của YuiVieOS.
//
// Trạng thái: khung sườn, chưa build trong CI (xem ui/README.md).
// Ba màn hình: trò chuyện (gõ tiếng Việt + tìm tri thức), đồ thị, khoá.
import 'package:flutter/material.dart';

import 'daemon_client.dart';

void main() => runApp(const YuiApp());

class YuiApp extends StatelessWidget {
  const YuiApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'App Yui',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFFDA251D)),
        useMaterial3: true,
      ),
      home: const HomePage(),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  final DaemonClient _daemon = DaemonClient();
  final TextEditingController _socket = TextEditingController(text: '/run/yuid.sock');
  final TextEditingController _input = TextEditingController();
  final TextEditingController _password = TextEditingController();

  final List<MapEntry<String, String>> _log = []; // (ai | người, nội dung)
  String _status = 'Chưa kết nối';
  int _tab = 0;
  bool _busy = false;

  @override
  void dispose() {
    _daemon.close();
    _socket.dispose();
    _input.dispose();
    _password.dispose();
    super.dispose();
  }

  void _note(String who, String text) =>
      setState(() => _log.add(MapEntry(who, text)));

  Future<void> _connect() async {
    setState(() => _status = 'Đang kết nối…');
    try {
      await _daemon.connect();
      final version = await _daemon.version();
      setState(() => _status = 'Đã kết nối · yuid $version');
      _note('yui', 'Xin chào! Mình là Yui. Gõ tiếng Việt bằng Telex cũng được.');
    } catch (e) {
      setState(() => _status = 'Lỗi: $e');
    }
  }

  Future<void> _send() async {
    final text = _input.text.trim();
    if (text.isEmpty || _busy) return;
    _input.clear();
    _note('bạn', text);
    setState(() => _busy = true);
    try {
      final viet = await _daemon.convertKeys(text);
      if (viet != text) _note('gõ', viet);
      final hits = await _daemon.search(viet);
      if (hits.isEmpty) {
        _note('yui', 'Mình chưa có gì về "${viet}" trong đồ thị. Dùng tab Đồ thị để thêm nhé.');
      } else {
        for (final hit in hits.take(5)) {
          _note('yui', '${hit['label'] ?? hit['id']} · ${hit['kind'] ?? 'note'}');
        }
      }
    } catch (e) {
      _note('yui', 'Lỗi: $e');
    } finally {
      setState(() => _busy = false);
    }
  }

  Future<void> _remember() async {
    final label = _input.text.trim();
    if (label.isEmpty) return;
    _input.clear();
    try {
      final id = 'note:${DateTime.now().millisecondsSinceEpoch}';
      await _daemon.remember(id, label);
      _note('yui', 'Đã ghi nhớ: $label');
    } catch (e) {
      _note('yui', 'Lỗi: $e');
    }
  }

  Future<void> _seal() async {
    final plain = _input.text.trim();
    if (plain.isEmpty || _password.text.isEmpty) {
      _note('yui', 'Cần nội dung và mật khẩu để niêm phong.');
      return;
    }
    try {
      final blob = await _daemon.seal(_password.text, plain);
      _note('yui', 'Đã niêm phong ${blob.length} ký tự base64. Giữ kỹ mật khẩu — không có nó thì không mở được.');
      _input.clear();
    } catch (e) {
      _note('yui', 'Lỗi: $e');
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('App Yui · YuiVieOS'),
        bottom: PreferredSize(
          preferredSize: const Size.fromHeight(28),
          child: Align(
            alignment: Alignment.centerLeft,
            child: Padding(
              padding: const EdgeInsets.only(left: 16, bottom: 6),
              child: Text(_status, style: Theme.of(context).textTheme.bodySmall),
            ),
          ),
        ),
      ),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 8, 12, 0),
            child: Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _socket,
                    decoration: const InputDecoration(
                      labelText: 'Socket của yuid',
                      isDense: true,
                      border: OutlineInputBorder(),
                    ),
                  ),
                ),
                const SizedBox(width: 8),
                FilledButton(onPressed: _connect, child: const Text('Kết nối')),
              ],
            ),
          ),
          Expanded(child: _tab == 2 ? _vaultView() : _chatView()),
          _inputBar(),
        ],
      ),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _tab,
        onDestinationSelected: (i) => setState(() => _tab = i),
        destinations: const [
          NavigationDestination(icon: Icon(Icons.chat_bubble_outline), label: 'Trò chuyện'),
          NavigationDestination(icon: Icon(Icons.hub_outlined), label: 'Đồ thị'),
          NavigationDestination(icon: Icon(Icons.lock_outline), label: 'Khoá'),
        ],
      ),
    );
  }

  Widget _chatView() {
    return ListView.builder(
      padding: const EdgeInsets.all(12),
      itemCount: _log.length,
      itemBuilder: (context, i) {
        final entry = _log[i];
        final mine = entry.key == 'bạn';
        return Align(
          alignment: mine ? Alignment.centerRight : Alignment.centerLeft,
          child: Container(
            margin: const EdgeInsets.symmetric(vertical: 4),
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
            constraints: const BoxConstraints(maxWidth: 420),
            decoration: BoxDecoration(
              color: mine
                  ? Theme.of(context).colorScheme.primaryContainer
                  : Theme.of(context).colorScheme.surfaceContainerHighest,
              borderRadius: BorderRadius.circular(12),
            ),
            child: Text(entry.value),
          ),
        );
      },
    );
  }

  Widget _vaultView() {
    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text('Niêm phong bằng scrypt + SHAKE-256 + HMAC (Tầng 4). '
              'Nội dung lấy từ ô nhập bên dưới.'),
          const SizedBox(height: 12),
          TextField(
            controller: _password,
            obscureText: true,
            decoration: const InputDecoration(
              labelText: 'Mật khẩu',
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 12),
          FilledButton.icon(
            onPressed: _seal,
            icon: const Icon(Icons.lock),
            label: const Text('Niêm phong'),
          ),
        ],
      ),
    );
  }

  Widget _inputBar() {
    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(12, 4, 12, 8),
        child: Row(
          children: [
            Expanded(
              child: TextField(
                controller: _input,
                onSubmitted: (_) => _tab == 2 ? _seal() : _send(),
                decoration: InputDecoration(
                  hintText: _tab == 2
                      ? 'Nội dung cần niêm phong'
                      : 'Gõ Telex: Tieengs Vieetj…',
                  border: const OutlineInputBorder(),
                  isDense: true,
                ),
              ),
            ),
            const SizedBox(width: 8),
            IconButton.filled(
              tooltip: 'Ghi nhớ vào đồ thị',
              onPressed: _remember,
              icon: const Icon(Icons.bookmark_add_outlined),
            ),
            const SizedBox(width: 8),
            IconButton.filled(
              tooltip: 'Gửi',
              onPressed: _busy ? null : (_tab == 2 ? _seal : _send),
              icon: _busy
                  ? const SizedBox(
                      width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2))
                  : const Icon(Icons.send),
            ),
          ],
        ),
      ),
    );
  }
}
