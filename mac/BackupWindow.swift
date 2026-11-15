import AppKit
import WebKit
import UniformTypeIdentifiers

// 两端运行卡片提供的同一套备份校验与恢复流程；Mac 使用系统文件对话框。
final class BackupWindow: NSObject, WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler, NSWindowDelegate {
    let window: NSWindow
    private let address: URL
    private let web: WKWebView
    var closed: (() -> Void)?

    static func requestAddress() -> URL? {
        var detail = "在卡片打开「设置 → 连接 → 网页设置」，输入屏幕上的网址。临时热点模式下，请先让 Mac 加入卡片热点。"
        let entry = NSTextField(string: "")
        entry.placeholderString = "例如 http://192.168.4.1"
        while PassportDialog(title: "资料备份", detail: detail, content: entry, confirm: "打开", cancel: "取消").run() {
            let value = entry.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
            let url = URLComponents(string: value.contains("://") ? value : "http://" + value)
            if let url, url.scheme == "http", let host = url.host,
               url.user == nil, url.password == nil, url.port == nil,
               url.path.isEmpty || url.path == "/", isCardHost(host) {
                return url.url
            }
            entry.removeFromSuperview()
            detail = "请输入卡片屏幕上显示的局域网 IPv4 网址。"
        }
        return nil
    }

    private static func isCardHost(_ host: String) -> Bool {
        let octets = host.split(separator: ".", omittingEmptySubsequences: false).compactMap { UInt8($0) }
        guard octets.count == 4 else { return false }
        return octets[0] == 10 || (octets[0] == 192 && octets[1] == 168)
            || (octets[0] == 172 && (16...31).contains(octets[1]))
            || (octets[0] == 169 && octets[1] == 254)
    }

    init(address: URL) {
        self.address = address
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        web = WKWebView(frame: .zero, configuration: config)
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 560),
                          styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        super.init()
        web.navigationDelegate = self
        web.uiDelegate = self
        web.configuration.userContentController.add(self, name: "passportBackup")
        window.title = "Passport · 资料备份"
        PassportUI.style(window)
        let root = window.contentView!
        let heading = PassportUI.label("资料备份", size: 18, color: .labelColor, weight: .semibold)
        let header = PassportUI.card([heading], inset: 12)
        let stack = PassportUI.column([header, web], spacing: 12)
        header.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        web.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        PassportUI.fill(stack, in: root, inset: 16)
        window.minSize = NSSize(width: 480, height: 390)
        window.delegate = self
        window.isReleasedWhenClosed = false
        window.center()
        web.load(URLRequest(url: address, timeoutInterval: 15))
    }

    private func isCard(_ url: URL?) -> Bool {
        url?.scheme == "http" && url?.host == address.host && url?.port == address.port
    }

    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        decisionHandler(isCard(action.request.url) && action.targetFrame?.isMainFrame == true ? .allow : .cancel)
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        webView.evaluateJavaScript("""
            if (document.getElementById('openBackup')) {
                document.getElementById('navDevice').click();
                document.getElementById('openBackup').click();
            }
            """, completionHandler: nil)
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        let dialog = PassportDialog(title: "暂时无法打开卡片",
            detail: "请确认 Mac 与卡片在同一网络，且机身的网页设置还开着。",
            confirm: "重试", cancel: "关闭")
        if dialog.run() { web.load(URLRequest(url: address, timeoutInterval: 15)) }
        else { window.close() }
    }

    func webView(_ webView: WKWebView, runOpenPanelWith parameters: WKOpenPanelParameters,
                 initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping ([URL]?) -> Void) {
        guard frame.isMainFrame, isCard(frame.request.url) else { completionHandler(nil); return }
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        panel.allowsMultipleSelection = false
        panel.beginSheetModal(for: window) { response in completionHandler(response == .OK ? panel.urls : nil) }
    }

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        guard message.frameInfo.isMainFrame, isCard(message.frameInfo.request.url),
              let payload = message.body as? [String: String], let filename = payload["filename"],
              filename.range(of: #"^Passport-[0-9]{4}-[0-9]{2}-[0-9]{2}\.json$"#, options: .regularExpression) != nil,
              let contents = payload["text"], contents.utf8.count <= 900000,
              window.attachedSheet == nil else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = filename
        panel.beginSheetModal(for: window) { [weak self] result in
            guard let self else { return }
            var status = "已取消导出"
            if result == .OK, let url = panel.url {
                do {
                    try Data(contents.utf8).write(to: url, options: .atomic)
                    status = "备份已保存在 Mac"
                } catch {
                    status = "备份保存失败，请选择可写的位置"
                }
            }
            // 状态只来自上面定义的固定短句。
            self.web.evaluateJavaScript("document.getElementById('backupResult').textContent = '\(status)';", completionHandler: nil)
        }
    }

    func windowWillClose(_ notification: Notification) {
        web.stopLoading()
        web.configuration.userContentController.removeScriptMessageHandler(forName: "passportBackup")
        closed?()
    }
}
