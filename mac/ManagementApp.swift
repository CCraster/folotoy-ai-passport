import AppKit
import Carbon
import ServiceManagement
import Darwin

let appIdentifier = "com.craster.passport-sync"
let supportURL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Passport Sync")
let appVersion = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "development"
let sourceCommit = Bundle.main.object(forInfoDictionaryKey: "PassportSourceCommit") as? String ?? "unknown"

private func isCodexExecutable(_ path: String) -> Bool {
    guard path.hasPrefix("/") else { return false }
    let file = URL(fileURLWithPath: path).resolvingSymlinksInPath()
    let attributes = try? file.resourceValues(forKeys: [.isRegularFileKey])
    return attributes?.isRegularFile == true && FileManager.default.isExecutableFile(atPath: file.path)
}

func loginState() -> String {
    switch SMAppService.mainApp.status {
    case .enabled: return "已开启"
    case .requiresApproval: return "等待系统允许"
    case .notRegistered: return "已关闭"
    case .notFound: return "应用尚未正确安装"
    @unknown default: return "状态未知"
    }
}

final class ManagementApp: NSObject, NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate {
    private var engine: PassportSync!
    private var window: NSWindow!
    private var statusItem: NSStatusItem!
    private var backupWindow: BackupWindow?
    private let taskLabel = NSTextField(labelWithString: "—")
    private var connectionLabel = NSTextField(labelWithString: "—")
    private var syncLabel = NSTextField(labelWithString: "—")
    private var quotaLabel = NSTextField(labelWithString: "—")
    private var resetLabel = NSTextField(labelWithString: "—")
    private var expiryLabel = NSTextField(wrappingLabelWithString: "")
    private var nextExpiryLabel = NSTextField(labelWithString: "")
    private var quotaNote = NSTextField(labelWithString: "")
    private var quotaMeter = QuotaMeter()
    private var loginSwitch: NSSwitch!
    private var errorLabel = NSTextField(wrappingLabelWithString: "")
    private var runtimeLabel = NSTextField(labelWithString: "")
    private var pauseButton: NSButton!
    private var syncButton: NSButton!
    private var lastDiagnostic = ""
    private var lockFD: Int32 = -1
    private var terminateSignal: DispatchSourceSignal?

    func applicationDidFinishLaunching(_ notification: Notification) {
        do {
            try FileManager.default.createDirectory(at: supportURL, withIntermediateDirectories: true)
            lockFD = open(supportURL.appendingPathComponent("app.lock").path, O_CREAT | O_RDWR | O_CLOEXEC, 0o600)
            guard lockFD >= 0 else { throw POSIXError(.EACCES) }
            guard flock(lockFD, LOCK_EX | LOCK_NB) == 0 else {
                DistributedNotificationCenter.default().postNotificationName(Notification.Name(appIdentifier + ".show"), object: nil)
                NSApp.terminate(nil); return
            }
            let savedCodex = UserDefaults.standard.string(forKey: "codexPath")
            let needsSetup = savedCodex.map(isCodexExecutable) != true
            if needsSetup {
                NSApp.setActivationPolicy(.regular)
                NSApp.activate(ignoringOtherApps: true)
            }
            guard let codex = needsSetup ? requestCodexPath() : savedCodex else {
                NSApp.terminate(nil); return
            }
            buildWindow()
            configureSync(codex)
            signal(SIGTERM, SIG_IGN)
            terminateSignal = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
            terminateSignal?.setEventHandler { NSApp.terminate(nil) }
            terminateSignal?.resume()
            buildMenu()
            DistributedNotificationCenter.default().addObserver(self, selector: #selector(showWindow), name: Notification.Name(appIdentifier + ".show"), object: nil)
            update()
            let login = NSAppleEventManager.shared().currentAppleEvent?
                .paramDescriptor(forKeyword: keyAEPropData)?.enumCodeValue == keyAELaunchedAsLogInItem
            if !CommandLine.arguments.contains("--background") && !login { showWindow() }
        } catch { alert(error.localizedDescription); NSApp.terminate(nil) }
    }

    private func requestCodexPath(current: String? = nil) -> String? {
        let home = FileManager.default.homeDirectoryForCurrentUser
        let path = ProcessInfo.processInfo.environment["PATH"] ?? ""
        let candidates = path.split(separator: ":").map { String($0) + "/codex" } + [
            "/opt/homebrew/bin/codex", "/usr/local/bin/codex", home.appendingPathComponent(".local/bin/codex").path,
        ]
        let field = NSTextField(string: current ?? candidates.first(where: isCodexExecutable) ?? "")
        field.placeholderString = "/opt/homebrew/bin/codex"
        field.setAccessibilityLabel("Codex CLI 路径")
        let instructions = "请先安装 Codex CLI 并运行 codex login。可在终端运行 command -v codex，将输出的完整路径填在这里。"
        var detail = instructions
        while PassportDialog(title: "连接 Codex CLI", detail: detail, content: field, confirm: "继续", cancel: "取消").run() {
            let path = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
            if isCodexExecutable(path) { return path }
            detail = "该位置没有可执行文件。\n\(instructions)"
        }
        return nil
    }

    private func configureSync(_ codex: String) {
        engine?.changed = nil
        engine?.stop()
        UserDefaults.standard.set(codex, forKey: "codexPath")
        engine = PassportSync(codex: codex)
        engine.changed = { [weak self] in self?.update() }
    }

    private func buildWindow() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 620, height: 550),
                          styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "Passport Sync"; window.delegate = self; window.isReleasedWhenClosed = false
        window.titleVisibility = .hidden
        PassportUI.style(window)
        window.center()
        let content = window.contentView!
        let stack = PassportUI.column([], spacing: 20)
        stack.translatesAutoresizingMaskIntoConstraints = false; content.addSubview(stack)
        NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 26),
            stack.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -26),
            stack.topAnchor.constraint(equalTo: content.topAnchor, constant: 16)])
        let mark = NSImageView(image: NSImage(size: NSSize(width: 40, height: 40), flipped: false) { rect in
            drawPassportMark(in: rect.insetBy(dx: 2, dy: 2), color: accent); return true
        })
        mark.widthAnchor.constraint(equalToConstant: 40).isActive = true
        mark.heightAnchor.constraint(equalToConstant: 40).isActive = true
        connectionLabel.font = .systemFont(ofSize: 12, weight: .medium)
        connectionLabel.textColor = accent
        let heading = PassportUI.row([mark, PassportUI.column([PassportUI.label("Passport", size: 25, color: .labelColor), PassportUI.label("FoloToy AI Passport", size: 11)], spacing: 3), NSView(), connectionLabel])
        stack.addArrangedSubview(heading); heading.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true

        quotaLabel.font = .monospacedDigitSystemFont(ofSize: 52, weight: .medium); quotaLabel.textColor = accent
        quotaNote.font = .systemFont(ofSize: 11); quotaNote.textColor = .secondaryLabelColor
        quotaMeter.heightAnchor.constraint(equalToConstant: 6).isActive = true
        let quota = PassportUI.card([PassportUI.label("CODEX 剩余额度", size: 11), quotaLabel, quotaMeter, quotaNote])
        quotaMeter.widthAnchor.constraint(equalTo: quota.widthAnchor, constant: -36).isActive = true
        resetLabel.font = .monospacedDigitSystemFont(ofSize: 40, weight: .medium); resetLabel.textColor = .labelColor
        nextExpiryLabel.font = .systemFont(ofSize: 12); nextExpiryLabel.textColor = .labelColor
        expiryLabel.font = .systemFont(ofSize: 11); expiryLabel.textColor = .secondaryLabelColor
        expiryLabel.maximumNumberOfLines = 2
        let resets = PassportUI.card([PassportUI.label("可用重置卡", size: 11), resetLabel, nextExpiryLabel, expiryLabel])
        let cards = PassportUI.row([quota, resets], spacing: 12); cards.distribution = .fillEqually; cards.alignment = .top
        quota.heightAnchor.constraint(equalToConstant: 210).isActive = true
        resets.heightAnchor.constraint(equalToConstant: 210).isActive = true
        stack.addArrangedSubview(cards); cards.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true

        let taskTitle = PassportUI.label("进行中的 Codex 任务", color: .labelColor, weight: .medium)
        taskLabel.font = .monospacedDigitSystemFont(ofSize: 12, weight: .medium)
        taskLabel.textColor = .secondaryLabelColor
        taskLabel.lineBreakMode = .byTruncatingMiddle
        taskLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 320).isActive = true
        let taskRow = PassportUI.row([taskTitle, NSView(), taskLabel])
        let tasks = PassportUI.card([taskRow], inset: 12)
        taskRow.widthAnchor.constraint(equalTo: tasks.widthAnchor, constant: -24).isActive = true
        stack.addArrangedSubview(tasks); tasks.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true

        syncButton = PassportUI.button("立即同步", self, #selector(syncNow), primary: true)
        pauseButton = PassportUI.button("暂停同步", self, #selector(togglePause))
        syncLabel.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular); syncLabel.textColor = .secondaryLabelColor
        let actions = PassportUI.row([syncButton, pauseButton, NSView(), syncLabel], spacing: 10)
        stack.addArrangedSubview(actions); actions.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true

        loginSwitch = NSSwitch(); loginSwitch.target = self; loginSwitch.action = #selector(toggleLogin)
        let launch = PassportUI.row([PassportUI.label("登录 Mac 时自动启动", size: 13, color: .labelColor), NSView(), loginSwitch])
        stack.addArrangedSubview(launch); launch.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        errorLabel.font = .systemFont(ofSize: 12); errorLabel.textColor = .systemOrange
        errorLabel.maximumNumberOfLines = 2; stack.addArrangedSubview(errorLabel)
        errorLabel.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        runtimeLabel.font = .systemFont(ofSize: 11); runtimeLabel.textColor = .tertiaryLabelColor
        let manage = PassportUI.button("管理…", self, #selector(manageApp(_:))); manage.widthAnchor.constraint(equalToConstant: 94).isActive = true
        let footer = PassportUI.row([runtimeLabel, NSView(), manage])
        stack.addArrangedSubview(footer); footer.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
    }
    private func command(_ title: String, _ symbol: String, _ action: Selector) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: "")
        item.target = self
        item.image = NSImage(systemSymbolName: symbol, accessibilityDescription: nil)
        return item
    }

    @objc private func manageApp(_ sender: NSButton) {
        let menu = NSMenu()
        menu.items = [
            command("Codex CLI…", "terminal", #selector(editCodexPath)),
            command("SSH 任务来源…", "network", #selector(editRemoteSource)),
            command("资料备份", "externaldrive", #selector(showBackup)),
            command("诊断信息", "waveform.path", #selector(diagnostics)),
            command("系统登录项", "gearshape", #selector(openLoginSettings)),
            .separator(),
            command("停用后台", "stop.circle", #selector(stopApp)),
            command("卸载…", "trash", #selector(uninstall)),
        ]
        menu.popUp(positioning: nil, at: NSPoint(x: 0, y: sender.isFlipped ? sender.bounds.maxY : sender.bounds.minY), in: sender)
    }

    private func buildMenu() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        statusItem.button?.image = passportMenuSymbol()
        let menu = NSMenu()
        menu.delegate = self
        menuNeedsUpdate(menu)
        statusItem.menu = menu
    }
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.autoenablesItems = false
        let sync = command("立即同步", "arrow.clockwise", #selector(syncNow))
        sync.isEnabled = engine.connected && !engine.paused
        menu.items = [
            command("打开 Passport Sync", "rectangle.on.rectangle", #selector(showWindow)),
            .separator(), sync,
            command(engine.paused ? "恢复同步" : "暂停同步", engine.paused ? "play" : "pause", #selector(togglePause)),
            command("资料备份", "externaldrive", #selector(showBackup)),
            .separator(),
            command("停用后台", "stop.circle", #selector(stopApp)),
        ]
    }

    @objc func showWindow() {
        guard window != nil else { return }
        NSApp.setActivationPolicy(.regular); window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
    }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool { showWindow(); return true }
    func windowWillClose(_ notification: Notification) {
        if backupWindow == nil { NSApp.setActivationPolicy(.accessory) }
    }

    @objc private func showBackup() {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
        if let backupWindow { backupWindow.window.makeKeyAndOrderFront(nil); return }
        guard let address = BackupWindow.requestAddress() else {
            if !window.isVisible { NSApp.setActivationPolicy(.accessory) }
            return
        }
        let backup = BackupWindow(address: address)
        backup.closed = { [weak self] in
            guard let self else { return }
            self.backupWindow = nil
            if !self.window.isVisible { NSApp.setActivationPolicy(.accessory) }
        }
        backupWindow = backup
        backup.window.makeKeyAndOrderFront(nil)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
    func applicationDidBecomeActive(_ notification: Notification) { if engine != nil { update() } }

    private func formatted(_ date: Date?, full: Bool = false) -> String {
        guard let date else { return "尚未同步" }
        let formatter = DateFormatter(); formatter.dateFormat = full ? "MM/dd HH:mm" : "HH:mm:ss"
        return formatter.string(from: date)
    }
    private func update() {
        connectionLabel.stringValue = engine.paused ? "已暂停" : engine.connection
        syncLabel.stringValue = engine.lastSync.map { "同步于 " + formatted($0) } ?? "等待首次同步"
        let latest: QuotaData? = engine.latest
        let windows: [QuotaWindow] = (latest?.windows ?? []).filter { $0.minutes > 0 }
        quotaLabel.stringValue = windows.isEmpty ? "—" : windows.map { "\($0.remaining)%" }.joined(separator: " / ")
        quotaLabel.font = .monospacedDigitSystemFont(ofSize: windows.count > 1 ? 31 : 52, weight: .medium)
        quotaMeter.value = Double(windows.first?.remaining ?? 0)
        if let first = windows.first {
            let duration = first.minutes >= 1440 ? "\(first.minutes / 1440) 天窗口" : "\(first.minutes / 60) 小时窗口"
            quotaNote.stringValue = duration + (first.resetAt > 0 ? " · " + formatted(Date(timeIntervalSince1970: Double(first.resetAt)), full: true) + " 重置" : "")
        } else { quotaNote.stringValue = "等待 Codex 数据" }
        resetLabel.stringValue = engine.latest?.resets.map { "\($0.availableCount) 张" } ?? "—"
        if let resets = engine.latest?.resets {
            let dates = resets.credits?.filter { $0.status == "available" }.compactMap(\.expiresAt).sorted() ?? []
            if let next = dates.first {
                nextExpiryLabel.stringValue = "\(resets.complete ? "最近" : "最近已知") " + formatted(Date(timeIntervalSince1970: Double(next)), full: true) + " 到期"
                let remaining = Double(next) - Date().timeIntervalSince1970
                nextExpiryLabel.textColor = remaining <= 86400 ? .systemRed : remaining <= 3 * 86400 ? .systemOrange : .labelColor
                expiryLabel.stringValue = dates.dropFirst().prefix(2).map { formatted(Date(timeIntervalSince1970: Double($0)), full: true) + " 到期" }.joined(separator: "\n")
            } else {
                nextExpiryLabel.stringValue = resets.availableCount == 0 ? "暂无可用重置卡" : "到期时间暂不可用"
                nextExpiryLabel.textColor = .secondaryLabelColor; expiryLabel.stringValue = ""
            }
        } else { nextExpiryLabel.stringValue = "等待同步"; expiryLabel.stringValue = "" }
        pauseButton.title = engine.paused ? "恢复同步" : "暂停同步"
        syncButton.isEnabled = engine.connected && !engine.paused
        loginSwitch.state = SMAppService.mainApp.status == .enabled ? .on : .off
        let approval = SMAppService.mainApp.status == .requiresApproval ? "请在系统登录项中允许 Passport Sync" : nil
        let hint = engine.connectionHint
        errorLabel.stringValue = hint ?? engine.error ?? approval ?? ""
        errorLabel.textColor = hint == nil ? .systemOrange : .secondaryLabelColor
        errorLabel.isHidden = errorLabel.stringValue.isEmpty
        connectionLabel.stringValue = "● " + connectionLabel.stringValue
        connectionLabel.textColor = engine.connected && !engine.paused ? accent : .secondaryLabelColor
        runtimeLabel.stringValue = "v\(appVersion)  ·  \(sourceCommit.prefix(7))"
        statusItem?.button?.contentTintColor = engine.connected && !engine.paused ? .labelColor : .secondaryLabelColor
        let local = engine.localTasks.map(String.init) ?? "—"
        if let host = engine.remoteHost {
            let remote = engine.remoteTasks.map(String.init) ?? (engine.remoteUnavailable ? "未同步" : "—")
            taskLabel.stringValue = "Mac \(local)  ·  \(host) \(remote)"
        } else {
            taskLabel.stringValue = local
        }
        taskLabel.toolTip = taskLabel.stringValue
        taskLabel.textColor = engine.remoteUnavailable ? .systemOrange : .secondaryLabelColor
        let tasks = engine.activeTasks.map { " · Codex 进行中 \($0)" } ?? ""
        statusItem?.button?.toolTip = "Passport Sync · \(connectionLabel.stringValue) · \(syncLabel.stringValue)" + tasks
        persistDiagnostic()
    }
    private func persistDiagnostic(stopped: Bool = false) {
        let record: [String: Any] = ["pid": ProcessInfo.processInfo.processIdentifier, "version": appVersion,
            "commit": sourceCommit, "connection": stopped ? "已停用" : (engine.paused ? "已暂停" : engine.connection),
            "paused": engine.paused, "lastSync": engine.lastSync?.timeIntervalSince1970 as Any? ?? NSNull(),
            "lastRead": engine.lastRead?.timeIntervalSince1970 as Any? ?? NSNull(),
            "activeTasks": engine.activeTasks as Any? ?? NSNull(),
            "localTasks": engine.localTasks as Any? ?? NSNull(),
            "remoteSSHHost": engine.remoteHost as Any? ?? NSNull(),
            "remoteTasks": engine.remoteTasks as Any? ?? NSNull(),
            "remoteUnavailable": engine.remoteUnavailable,
            "cardTasks": engine.acknowledgedTasks as Any? ?? NSNull(),
            "lastCompletionAcknowledged": engine.lastCompletionAcknowledged?.timeIntervalSince1970 as Any? ?? NSNull(),
            "login": loginState(), "error": engine.error as Any? ?? NSNull()]
        do {
            let data = try JSONSerialization.data(withJSONObject: record, options: [.prettyPrinted, .sortedKeys])
            try data.write(to: supportURL.appendingPathComponent("status.json"), options: .atomic)
            let line = "\(connectionLabel.stringValue) | \(formatted(engine.lastSync)) | \(engine.error ?? "OK")"
            if line != lastDiagnostic {
                lastDiagnostic = line
                let log = supportURL.appendingPathComponent("sync.log")
                var contents = (try? Data(contentsOf: log)) ?? Data()
                if contents.count > 128 * 1024 { contents = Data(contents.suffix(64 * 1024)) }
                contents.append(Data("\(ISO8601DateFormatter().string(from: Date())) \(line)\n".utf8))
                try contents.write(to: log, options: .atomic)
            }
        } catch { errorLabel.stringValue = "诊断记录保存失败：\(error.localizedDescription)" }
    }
    private func alert(_ message: String) {
        _ = PassportDialog(title: "Passport Sync", detail: message).run()
    }
    @objc private func syncNow() { engine.refresh(); engine.refreshRemoteTasks() }
    @objc private func togglePause() { engine.setPaused(!engine.paused) }
    @objc private func toggleLogin() {
        do {
            if SMAppService.mainApp.status == .enabled { try SMAppService.mainApp.unregister() }
            else { try SMAppService.mainApp.register() }
        } catch { alert("登录启动设置失败：\(error.localizedDescription)") }
        update()
    }
    @objc private func openLoginSettings() { SMAppService.openSystemSettingsLoginItems() }
    @objc private func editCodexPath() {
        let current = UserDefaults.standard.string(forKey: "codexPath")
        guard let path = requestCodexPath(current: current), path != current else { return }
        configureSync(path)
        update()
    }
    @objc private func editRemoteSource() {
        let field = NSTextField(string: engine.remoteHost ?? "")
        field.placeholderString = "SSH 别名或主机名"
        field.setAccessibilityLabel("SSH 别名或主机名")
        var detail = "填写终端可以免交互连接的 SSH 别名或主机名。留空仅统计这台 Mac。"
        while PassportDialog(title: "SSH 任务来源", detail: detail, content: field,
                             confirm: "保存", cancel: "取消").run() {
            let host = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
            if host.isEmpty || isValidSSHHost(host) {
                engine.setRemoteHost(host.isEmpty ? nil : host)
                return
            }
            detail = "请输入 SSH 别名或主机名。用户名、端口和跳板机可在 ~/.ssh/config 配置。"
        }
    }
    @objc private func diagnostics() {
        let details = """
        版本  \(appVersion) · \(sourceCommit.prefix(7))
        卡片  \(engine.connection)
        最近同步  \(formatted(engine.lastSync, full: true))
        进行中的 Codex 任务  \(taskLabel.stringValue)
        卡片已确认  \(engine.acknowledgedTasks.map(String.init) ?? "—")
        完成提醒  \(engine.lastCompletionAcknowledged.map { formatted($0, full: true) } ?? "—")
        登录启动  \(loginState())
        \(engine.error ?? "运行正常")
        """
        if PassportDialog(title: "运行诊断", detail: details, confirm: "打开日志目录", cancel: "关闭").run() {
            NSWorkspace.shared.open(supportURL)
        }
    }
    @objc private func stopApp() {
        do {
            if SMAppService.mainApp.status != .notRegistered { try SMAppService.mainApp.unregister() }
            engine.setPaused(true); persistDiagnostic(stopped: true); NSApp.terminate(nil)
        } catch { alert("停用失败：\(error.localizedDescription)") }
    }
    @objc private func uninstall() {
        let clear = NSButton(checkboxWithTitle: "同时清理本机配对选择、偏好和日志", target: nil, action: nil)
        guard PassportDialog(title: "卸载 Passport Sync", detail: "停止同步并移除登录启动和应用。",
                             content: clear, confirm: "卸载", cancel: "取消", destructive: true).run() else { return }
        do {
            guard Bundle.main.bundleURL.path == "/Applications/Passport Sync.app" else {
                throw NSError(domain: appIdentifier, code: 2, userInfo: [NSLocalizedDescriptionKey: "请从应用程序目录打开已安装版本再卸载。"])
            }
            if SMAppService.mainApp.status != .notRegistered { try SMAppService.mainApp.unregister() }
            engine.setPaused(true); persistDiagnostic(stopped: true)
            try FileManager.default.trashItem(at: Bundle.main.bundleURL, resultingItemURL: nil)
            if clear.state == .on {
                UserDefaults.standard.removePersistentDomain(forName: appIdentifier)
                try FileManager.default.removeItem(at: supportURL)
            }
            NSApp.terminate(nil)
        } catch { self.alert("卸载未完成：\(error.localizedDescription)") }
    }
    func applicationWillTerminate(_ notification: Notification) {
        if engine != nil { engine.stop() }
    }
}

@main struct PassportMain {
    static func main() throws {
        let args = CommandLine.arguments
        if args.contains("--self-test") { try testQuotaData(); return }
        if args.count == 3 && args[1] == "--check-codex" {
            let data = try readLimits(codex: args[2])
            print("Codex: \(data.windows.filter { $0.minutes > 0 }.count) window(s), resets: \(data.resets.map { String($0.availableCount) } ?? "unknown")")
            return
        }
        if args.count == 3 && args[1] == "--configure" {
            guard isCodexExecutable(args[2]) else { exit(2) }
            UserDefaults.standard.set(args[2], forKey: "codexPath"); return
        }
        if args.contains("--register-login") {
            if SMAppService.mainApp.status != .enabled { try SMAppService.mainApp.register() }
            print(loginState()); return
        }
        if args.contains("--unregister-login") {
            if SMAppService.mainApp.status != .notRegistered { try SMAppService.mainApp.unregister() }; return
        }
        if args.contains("--login-enabled") {
            print(SMAppService.mainApp.status == .enabled || SMAppService.mainApp.status == .requiresApproval ? "true" : "false")
            return
        }
        if args.contains("--status") {
            var record = (try? JSONSerialization.jsonObject(with: Data(contentsOf: supportURL.appendingPathComponent("status.json")))) as? [String: Any] ?? [:]
            record["login"] = loginState()
            record["running"] = (record["pid"] as? Int32).map { kill($0, 0) == 0 } ?? false
            print(String(data: try JSONSerialization.data(withJSONObject: record, options: [.prettyPrinted, .sortedKeys]), encoding: .utf8)!)
            return
        }
        let app = NSApplication.shared; let delegate = ManagementApp()
        app.setActivationPolicy(.accessory); app.delegate = delegate
        withExtendedLifetime(delegate) { app.run() }
    }
}
