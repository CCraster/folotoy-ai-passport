import AppKit
import WebKit
import UniformTypeIdentifiers

// 复用卡片的配置网页，文件打开与保存由 macOS 原生面板处理。
final class BackupWindow: NSObject, WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler, NSWindowDelegate {
    enum Destination {
        case setup, backup

        var title: String {
            switch self {
            case .setup: return appText("设备配置", "Device Setup")
            case .backup: return appText("备份与恢复", "Backup & Restore")
            }
        }
    }

    let window: NSWindow
    private var destination: Destination
    private let heading = PassportUI.label("", size: 18, color: .labelColor, weight: .semibold)
    private let address: URL
    private let web: WKWebView
    var closed: (() -> Void)?

    static func requestAddress(_ destination: Destination) -> URL? {
        var detail = appText(
            "请让 Mac 和卡片连接同一局域网。在卡片进入「设置 → 连接 → 设备配置」，输入屏幕上的网址和访问码。也可以让 Mac 加入卡片的临时热点。",
            "Connect your Mac and Passport to the same network. On the Passport, open Settings → Connections → Device Setup, then enter the URL and access code shown there. You can also join the Passport hotspot on this Mac.")
        let entry = NSTextField(string: "")
        entry.placeholderString = appText("例如 http://192.168.1.10", "For example, http://192.168.1.10")
        while PassportDialog(title: destination.title, detail: detail, content: entry,
                             confirm: appText("打开", "Open"), cancel: appText("取消", "Cancel")).run() {
            let value = entry.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
            let url = URLComponents(string: value.contains("://") ? value : "http://" + value)
            if let url, url.scheme == "http", let host = url.host,
               url.user == nil, url.password == nil, url.port == nil,
               url.path.isEmpty || url.path == "/", isCardHost(host) {
                return url.url
            }
            entry.removeFromSuperview()
            detail = appText("请输入卡片屏幕上显示的局域网 IPv4 网址。", "Enter the local IPv4 address shown on your Passport.")
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

    init(address: URL, destination: Destination) {
        self.address = address
        self.destination = destination
        let config = WKWebViewConfiguration()
        config.websiteDataStore = .nonPersistent()
        web = WKWebView(frame: .zero, configuration: config)
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 560),
                          styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        super.init()
        web.navigationDelegate = self
        web.uiDelegate = self
        web.configuration.userContentController.add(self, name: "passportBackup")
        updateTitle()
        PassportUI.style(window)
        let root = window.contentView!
        let header = PassportUI.card([heading], inset: 12)
        let stack = PassportUI.column([header, web], spacing: 12)
        header.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        web.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        PassportUI.fill(stack, in: root, inset: 16)
        window.minSize = NSSize(width: 480, height: 390)
        window.delegate = self
        window.isReleasedWhenClosed = false
        window.center()
        openAddress()
    }

    private func openAddress() {
        var parts = URLComponents(url: address, resolvingAgainstBaseURL: false)!
        parts.fragment = "locale=\(AppLanguage.saved().locale())"
        guard let url = parts.url else { return }
        web.load(URLRequest(url: url, timeoutInterval: 15))
    }

    private func updateTitle() {
        heading.stringValue = destination.title
        window.title = "Passport · " + destination.title
    }

    func show(_ destination: Destination) {
        self.destination = destination
        updateTitle()
        showDestination()
        window.makeKeyAndOrderFront(nil)
    }

    func refreshLanguage() {
        updateTitle()
        let locale = AppLanguage.saved().locale()
        if web.url == nil { openAddress(); return }
        web.evaluateJavaScript("""
            const locator = new URLSearchParams(location.hash.slice(1));
            locator.set('locale', '\(locale)');
            location.hash = locator.toString();
            location.reload();
            """, completionHandler: nil)
    }

    private func isCard(_ url: URL?) -> Bool {
        url?.scheme == "http" && url?.host == address.host && url?.port == address.port
    }

    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        decisionHandler(isCard(action.request.url) && action.targetFrame?.isMainFrame == true ? .allow : .cancel)
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) { showDestination() }

    private func showDestination() {
        web.evaluateJavaScript("""
            Promise.resolve(typeof passportLanguageReady === 'undefined' ? null : passportLanguageReady).then(() => {
                const dialog = document.getElementById('backupDialog');
                if (!dialog) return;
                if (\(destination == .backup)) {
                    document.getElementById('navDevice').click();
                    if (!dialog.open) document.getElementById('openBackup').click();
                } else if (dialog.open) {
                    dialog.close();
                }
            });
            void 0;
            """, completionHandler: nil)
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        if (error as NSError).code == NSURLErrorCancelled { return }
        let dialog = PassportDialog(title: appText("暂时无法打开卡片", "Cannot Open Passport"),
            detail: appText("请确认 Mac 与卡片在同一网络，且机身的设备配置还开着。",
                            "Check that both devices are on the same network and Device Setup is still open on the Passport."),
            confirm: appText("重试", "Retry"), cancel: appText("关闭", "Close"))
        if dialog.run() { openAddress() }
        else { window.close() }
    }

    func webView(_ webView: WKWebView, runOpenPanelWith parameters: WKOpenPanelParameters,
                 initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping ([URL]?) -> Void) {
        guard frame.isMainFrame, isCard(frame.request.url) else { completionHandler(nil); return }
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = parameters.allowsMultipleSelection
        panel.canChooseDirectories = parameters.allowsDirectories
        panel.beginSheetModal(for: window) { response in completionHandler(response == .OK ? panel.urls : nil) }
    }

    func webView(_ webView: WKWebView, runJavaScriptConfirmPanelWithMessage message: String,
                 initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping (Bool) -> Void) {
        guard frame.isMainFrame, isCard(frame.request.url) else { completionHandler(false); return }
        let dialog = PassportDialog(title: destination.title, detail: message,
                                    confirm: appText("确认", "Confirm"), cancel: appText("取消", "Cancel"))
        completionHandler(dialog.run())
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
            var status = ("已取消导出", "Export canceled.")
            if result == .OK, let url = panel.url {
                do {
                    try Data(contents.utf8).write(to: url, options: .atomic)
                    status = ("备份已保存在 Mac", "Backup saved on this Mac.")
                } catch {
                    status = ("备份保存失败，请选择可写的位置", "Unable to save the backup. Choose a writable folder.")
                }
            }
            // 状态只来自上面定义的固定短句。
            self.web.evaluateJavaScript("document.getElementById('backupResult').textContent = document.documentElement.lang === 'en' ? '\(status.1)' : '\(status.0)';", completionHandler: nil)
        }
    }

    func windowWillClose(_ notification: Notification) {
        web.stopLoading()
        web.configuration.userContentController.removeScriptMessageHandler(forName: "passportBackup")
        closed?()
    }
}
