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
    case .enabled: return appText("已开启", "On")
    case .requiresApproval: return appText("等待系统允许", "Awaiting system approval")
    case .notRegistered: return appText("已关闭", "Off")
    case .notFound: return appText("找不到登录启动项", "Login item not found")
    @unknown default: return appText("状态未知", "Unknown")
    }
}

final class ManagementApp: NSObject, NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate {
    private var engine: PassportSync!
    private var window: NSWindow!
    private var statusItem: NSStatusItem!
    private var backupWindow: BackupWindow?
    private var taskLabel = NSTextField(labelWithString: "—")
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
        field.setAccessibilityLabel(appText("Codex CLI 路径", "Codex CLI path"))
        let instructions = appText("请先安装 Codex CLI 并运行 codex login。可在终端运行 command -v codex，将输出的完整路径填在这里。", "Install the Codex CLI and run codex login. In Terminal, run command -v codex and paste the full path here.")
        var detail = instructions
        while PassportDialog(title: appText("连接 Codex CLI", "Connect Codex CLI"), detail: detail, content: field, confirm: appText("继续", "Continue"), cancel: appText("取消", "Cancel")).run() {
            let path = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
            if isCodexExecutable(path) { return path }
            detail = appText("该位置没有可执行文件。\n\(instructions)", "No executable was found at that path.\n\(instructions)")
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
        taskLabel = NSTextField(labelWithString: "—")
        connectionLabel = NSTextField(labelWithString: "—")
        syncLabel = NSTextField(labelWithString: "—")
        quotaLabel = NSTextField(labelWithString: "—")
        resetLabel = NSTextField(labelWithString: "—")
        expiryLabel = NSTextField(wrappingLabelWithString: "")
        nextExpiryLabel = NSTextField(labelWithString: "")
        quotaNote = NSTextField(labelWithString: "")
        quotaMeter = QuotaMeter()
        errorLabel = NSTextField(wrappingLabelWithString: "")
        runtimeLabel = NSTextField(labelWithString: "")
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 660, height: 532),
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
        let heading = PassportUI.row([mark, PassportUI.column([PassportUI.label("Passport Sync", size: 25, color: .labelColor), PassportUI.label("FoloToy AI Passport", size: 11)], spacing: 3), NSView(), connectionLabel])
        stack.addArrangedSubview(heading); heading.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true

        quotaLabel.font = .monospacedDigitSystemFont(ofSize: 52, weight: .medium); quotaLabel.textColor = accent
        quotaNote.font = .systemFont(ofSize: 11); quotaNote.textColor = .secondaryLabelColor
        quotaMeter.heightAnchor.constraint(equalToConstant: 6).isActive = true
        let quota = PassportUI.card([PassportUI.label(appText("CODEX 剩余额度", "CODEX QUOTA LEFT"), size: 11), quotaLabel, quotaMeter, quotaNote])
        quotaMeter.widthAnchor.constraint(equalTo: quota.widthAnchor, constant: -36).isActive = true
        resetLabel.font = .monospacedDigitSystemFont(ofSize: 40, weight: .medium); resetLabel.textColor = .labelColor
        nextExpiryLabel.font = .systemFont(ofSize: 12); nextExpiryLabel.textColor = .labelColor
        expiryLabel.font = .systemFont(ofSize: 11); expiryLabel.textColor = .secondaryLabelColor
        expiryLabel.maximumNumberOfLines = 2
        let resets = PassportUI.card([PassportUI.label(appText("可用重置卡", "AVAILABLE RESET CARDS"), size: 11), resetLabel, nextExpiryLabel, expiryLabel])
        let cards = PassportUI.row([quota, resets], spacing: 12); cards.distribution = .fillEqually; cards.alignment = .top
        quota.heightAnchor.constraint(equalToConstant: 210).isActive = true
        resets.heightAnchor.constraint(equalToConstant: 210).isActive = true
        stack.addArrangedSubview(cards); cards.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true

        let taskTitle = PassportUI.label(appText("进行中的 Codex 任务", "Active Codex tasks"), color: .labelColor, weight: .medium)
        taskLabel.font = .monospacedDigitSystemFont(ofSize: 12, weight: .medium)
        taskLabel.textColor = .secondaryLabelColor
        taskLabel.lineBreakMode = .byTruncatingMiddle
        taskLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 320).isActive = true
        let taskRow = PassportUI.row([taskTitle, NSView(), taskLabel])
        let tasks = PassportUI.card([taskRow], inset: 12)
        taskRow.widthAnchor.constraint(equalTo: tasks.widthAnchor, constant: -24).isActive = true
        stack.addArrangedSubview(tasks); tasks.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true

        syncButton = PassportUI.button(appText("立即同步", "Sync Now"), self, #selector(syncNow), primary: true)
        pauseButton = PassportUI.button(appText("暂停同步", "Pause Sync"), self, #selector(togglePause))
        syncLabel.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular); syncLabel.textColor = .secondaryLabelColor
        let actions = PassportUI.row([syncButton, pauseButton, NSView(), syncLabel], spacing: 10)
        stack.addArrangedSubview(actions); actions.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true

        errorLabel.font = .systemFont(ofSize: 12); errorLabel.textColor = .systemOrange
        errorLabel.maximumNumberOfLines = 2; stack.addArrangedSubview(errorLabel)
        errorLabel.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        runtimeLabel.font = .systemFont(ofSize: 11); runtimeLabel.textColor = .tertiaryLabelColor
        let setup = PassportUI.button(appText("设备配置…", "Device Setup…"), self, #selector(showDeviceSetup))
        let settings = PassportUI.button(appText("应用设置…", "App Settings…"), self, #selector(showAppSettings))
        let footer = PassportUI.row([runtimeLabel, NSView(), setup, settings])
        stack.addArrangedSubview(footer); footer.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
    }
    private func command(_ title: String, _ symbol: String, _ action: Selector) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: "")
        item.target = self
        item.image = NSImage(systemSymbolName: symbol, accessibilityDescription: nil)
        return item
    }

    private func managementMenu() -> NSMenuItem {
        let title = appText("管理", "Management")
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        let submenu = NSMenu(title: title)
        submenu.items = [
            command(appText("诊断信息", "Diagnostics"), "waveform.path", #selector(diagnostics)),
            .separator(),
            command(appText("停用后台", "Disable Background Sync"), "stop.circle", #selector(stopApp)),
            command(appText("卸载…", "Uninstall…"), "trash", #selector(uninstall)),
        ]
        item.submenu = submenu
        return item
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
        menu.items = statusMenuItems(paused: engine.paused, connected: engine.connected)
    }

    private func statusMenuItems(paused: Bool, connected: Bool) -> [NSMenuItem] {
        let sync = command(appText("立即同步", "Sync Now"), "arrow.clockwise", #selector(syncNow))
        sync.isEnabled = connected && !paused
        return [
            command(appText("打开 Passport Sync", "Open Passport Sync"), "rectangle.on.rectangle", #selector(showWindow)),
            .separator(), sync,
            command(paused ? appText("恢复同步", "Resume Sync") : appText("暂停同步", "Pause Sync"), paused ? "play" : "pause", #selector(togglePause)),
            .separator(),
            command(appText("设备配置…", "Device Setup…"), "slider.horizontal.3", #selector(showDeviceSetup)),
            command(appText("备份与恢复…", "Backup & Restore…"), "externaldrive", #selector(showBackup)),
            command(appText("应用设置…", "App Settings…"), "gearshape", #selector(showAppSettings)),
            managementMenu(),
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

    @objc private func showDeviceSetup() { showDevice(.setup) }
    @objc private func showBackup() { showDevice(.backup) }

    private func showDevice(_ destination: BackupWindow.Destination) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
        if let backupWindow {
            backupWindow.show(destination)
            return
        }
        guard let address = BackupWindow.requestAddress(destination) else {
            if !window.isVisible { NSApp.setActivationPolicy(.accessory) }
            return
        }
        let backup = BackupWindow(address: address, destination: destination)
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

    @objc func showAppSettings() {
        showWindow()
        let language = NSPopUpButton()
        language.addItems(withTitles: AppLanguage.allCases.map(\.title))
        language.selectItem(at: AppLanguage.allCases.firstIndex(of: AppLanguage.saved()) ?? 0)
        let launch = NSSwitch()
        launch.target = self
        launch.action = #selector(toggleLogin)
        launch.state = SMAppService.mainApp.status == .enabled ? .on : .off
        loginSwitch = launch
        defer { loginSwitch = nil }

        let languageRow = PassportUI.row([
            PassportUI.label(appText("界面语言", "App language"), color: .labelColor), NSView(), language,
        ])
        let loginRow = PassportUI.row([
            PassportUI.label(appText("登录 Mac 时自动启动", "Launch at login"), color: .labelColor), NSView(), launch,
        ])
        let cli = PassportUI.button(appText("Codex CLI…", "Codex CLI…"), self, #selector(editCodexPath))
        let ssh = PassportUI.button(appText("SSH 任务来源…", "SSH Task Source…"), self, #selector(editRemoteSource))
        let systemLogin = PassportUI.button(appText("系统登录项", "System Login Items"), self, #selector(openLoginSettings))
        let contents = PassportUI.column([languageRow, loginRow, cli, ssh, systemLogin], spacing: 12)
        languageRow.widthAnchor.constraint(equalTo: contents.widthAnchor).isActive = true
        loginRow.widthAnchor.constraint(equalTo: contents.widthAnchor).isActive = true
        let dialog = PassportDialog(title: appText("应用设置", "App Settings"),
            detail: appText("应用语言与卡片语言分别保存。", "App and device languages are saved separately."),
            content: contents, confirm: appText("完成", "Done"))
        guard dialog.run(), AppLanguage.allCases.indices.contains(language.indexOfSelectedItem) else { return }
        let chosen = AppLanguage.allCases[language.indexOfSelectedItem]
        guard chosen != AppLanguage.saved() else { return }
        chosen.save()
        let old = window!
        buildWindow()
        window.setFrameOrigin(old.frame.origin)
        old.delegate = nil
        old.close()
        NSApp.mainMenu = PassportMain.mainMenu()
        backupWindow?.refreshLanguage()
        update()
        showWindow()
    }

    private func formatted(_ date: Date?, full: Bool = false) -> String {
        guard let date else { return appText("尚未同步", "No sync yet") }
        let formatter = DateFormatter(); formatter.dateFormat = full ? "MM/dd HH:mm" : "HH:mm:ss"
        return formatter.string(from: date)
    }
    private func update() {
        connectionLabel.stringValue = engine.paused ? appText("已暂停", "Paused") : engine.connectionTitle
        syncLabel.stringValue = engine.lastSync.map { appText("同步于 ", "Synced at ") + formatted($0) } ?? appText("等待首次同步", "Waiting for first sync")
        let latest: QuotaData? = engine.latest
        let windows: [QuotaWindow] = (latest?.windows ?? []).filter { $0.minutes > 0 }
        quotaLabel.stringValue = windows.isEmpty ? "—" : windows.map { "\($0.remaining)%" }.joined(separator: " / ")
        quotaLabel.font = .monospacedDigitSystemFont(ofSize: windows.count > 1 ? 31 : 52, weight: .medium)
        quotaMeter.value = Double(windows.first?.remaining ?? 0)
        if let first = windows.first {
            let duration = first.minutes >= 1440 ? appText("\(first.minutes / 1440) 天窗口", "\(first.minutes / 1440)-day window") : appText("\(first.minutes / 60) 小时窗口", "\(first.minutes / 60)-hour window")
            let resetTime = formatted(Date(timeIntervalSince1970: Double(first.resetAt)), full: true)
            quotaNote.stringValue = duration + (first.resetAt > 0 ?
                appText(" · " + resetTime + " 重置", " · Resets " + resetTime) : "")
        } else { quotaNote.stringValue = appText("等待 Codex 数据", "Waiting for Codex data") }
        resetLabel.stringValue = engine.latest?.resets.map { appText("\($0.availableCount) 张", "\($0.availableCount)") } ?? "—"
        if let resets = engine.latest?.resets {
            let dates = resets.credits?.filter { $0.status == "available" }.compactMap(\.expiresAt).sorted() ?? []
            if let next = dates.first {
                let prefix = resets.complete ? appText("最近", "Next expiry") : appText("最近已知", "Last known expiry")
                let time = formatted(Date(timeIntervalSince1970: Double(next)), full: true)
                nextExpiryLabel.stringValue = appText(prefix + " " + time + " 到期", prefix + " · " + time)
                let remaining = Double(next) - Date().timeIntervalSince1970
                nextExpiryLabel.textColor = remaining <= 86400 ? .systemRed : remaining <= 3 * 86400 ? .systemOrange : .labelColor
                expiryLabel.stringValue = dates.dropFirst().prefix(2).map { formatted(Date(timeIntervalSince1970: Double($0)), full: true) + appText(" 到期", " expires") }.joined(separator: "\n")
            } else {
                nextExpiryLabel.stringValue = resets.availableCount == 0 ? appText("暂无可用重置卡", "No reset cards") : appText("到期时间暂不可用", "Expiry unavailable")
                nextExpiryLabel.textColor = .secondaryLabelColor; expiryLabel.stringValue = ""
            }
        } else { nextExpiryLabel.stringValue = appText("等待同步", "Waiting for sync"); expiryLabel.stringValue = "" }
        pauseButton.title = engine.paused ? appText("恢复同步", "Resume Sync") : appText("暂停同步", "Pause Sync")
        syncButton.isEnabled = engine.connected && !engine.paused
        loginSwitch?.state = SMAppService.mainApp.status == .enabled ? .on : .off
        let approval = SMAppService.mainApp.status == .requiresApproval ? appText("请在系统登录项中允许 Passport Sync", "Allow Passport Sync in System Login Items.") : nil
        let hint = engine.connectionHint
        errorLabel.stringValue = hint ?? engine.error ?? approval ?? ""
        errorLabel.textColor = hint == nil ? .systemOrange : .secondaryLabelColor
        errorLabel.isHidden = errorLabel.stringValue.isEmpty
        connectionLabel.stringValue = "● " + connectionLabel.stringValue
        connectionLabel.textColor = engine.connected && !engine.paused ? accent : .secondaryLabelColor
        runtimeLabel.stringValue = "v\(appVersion)  ·  \(sourceCommit.prefix(7))"
        let local = engine.localTasks.map(String.init) ?? "—"
        if let host = engine.remoteHost {
            let remote = engine.remoteTasks.map(String.init) ?? (engine.remoteUnavailable ? appText("未同步", "Unavailable") : "—")
            taskLabel.stringValue = "Mac \(local)  ·  \(host) \(remote)"
        } else {
            taskLabel.stringValue = local
        }
        taskLabel.toolTip = taskLabel.stringValue
        taskLabel.textColor = engine.remoteUnavailable ? .systemOrange : .secondaryLabelColor
        let tasks = engine.activeTasks.map { appText(" · Codex 进行中 \($0)", " · Active Codex tasks: \($0)") } ?? ""
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
        } catch { errorLabel.stringValue = appText("诊断记录保存失败：\(error.localizedDescription)", "Unable to save diagnostics: \(error.localizedDescription)") }
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
        } catch { alert(appText("登录启动设置失败：\(error.localizedDescription)", "Unable to update login launch: \(error.localizedDescription)")) }
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
        field.placeholderString = appText("SSH 别名或主机名", "SSH alias or hostname")
        field.setAccessibilityLabel(appText("SSH 别名或主机名", "SSH alias or hostname"))
        var detail = appText("填写终端可以免交互连接的 SSH 别名或主机名。留空仅统计这台 Mac。", "Enter an SSH alias or hostname that connects without interactive prompts. Leave it empty to count tasks on this Mac.")
        while PassportDialog(title: appText("SSH 任务来源", "SSH Task Source"), detail: detail, content: field,
                             confirm: appText("保存", "Save"), cancel: appText("取消", "Cancel")).run() {
            let host = field.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
            if host.isEmpty || isValidSSHHost(host) {
                engine.setRemoteHost(host.isEmpty ? nil : host)
                return
            }
            detail = appText("请输入 SSH 别名或主机名。用户名、端口和跳板机可在 ~/.ssh/config 配置。", "Enter an SSH alias or hostname. Configure user, port and jump host in ~/.ssh/config.")
        }
    }
    @objc private func diagnostics() {
        let details = [
            (appText("版本", "Version"), "\(appVersion) · \(sourceCommit.prefix(7))"),
            (appText("卡片", "Device"), engine.connectionTitle),
            (appText("最近同步", "Last sync"), formatted(engine.lastSync, full: true)),
            (appText("进行中的 Codex 任务", "Active Codex tasks"), taskLabel.stringValue),
            (appText("卡片已确认", "Confirmed by device"), engine.acknowledgedTasks.map(String.init) ?? "—"),
            (appText("完成提醒", "Completion alert"), engine.lastCompletionAcknowledged.map { formatted($0, full: true) } ?? "—"),
            (appText("登录启动", "Launch at login"), loginState()),
        ].map { $0.0 + "  " + $0.1 }.joined(separator: "\n") + "\n" +
            (engine.error ?? appText("运行正常", "Running normally"))
        if PassportDialog(title: appText("运行诊断", "Sync Diagnostics"), detail: details, confirm: appText("打开日志目录", "Open Logs Folder"), cancel: appText("关闭", "Close")).run() {
            NSWorkspace.shared.open(supportURL)
        }
    }
    @objc private func stopApp() {
        do {
            if SMAppService.mainApp.status != .notRegistered { try SMAppService.mainApp.unregister() }
            engine.setPaused(true); persistDiagnostic(stopped: true); NSApp.terminate(nil)
        } catch { alert(appText("停用失败：\(error.localizedDescription)", "Unable to disable background sync: \(error.localizedDescription)")) }
    }
    @objc private func uninstall() {
        let clear = NSButton(checkboxWithTitle: appText("同时清理本机配对选择、偏好和日志", "Also clear local pairing selections, preferences and logs"), target: nil, action: nil)
        guard PassportDialog(title: appText("卸载 Passport Sync", "Uninstall Passport Sync"), detail: appText("停止同步并移除登录启动和应用。", "Stop sync and remove the application and its login item."),
                             content: clear, confirm: appText("卸载", "Uninstall"), cancel: appText("取消", "Cancel"), destructive: true).run() else { return }
        do {
            guard Bundle.main.bundleURL.path == "/Applications/Passport Sync.app" else {
                throw NSError(domain: appIdentifier, code: 2, userInfo: [NSLocalizedDescriptionKey: appText("请从应用程序目录打开已安装版本再卸载。", "Open the installed copy in Applications before uninstalling.")])
            }
            if SMAppService.mainApp.status != .notRegistered { try SMAppService.mainApp.unregister() }
            engine.setPaused(true); persistDiagnostic(stopped: true)
            try FileManager.default.trashItem(at: Bundle.main.bundleURL, resultingItemURL: nil)
            if clear.state == .on {
                UserDefaults.standard.removePersistentDomain(forName: appIdentifier)
                try FileManager.default.removeItem(at: supportURL)
            }
            NSApp.terminate(nil)
        } catch { self.alert(appText("卸载未完成：\(error.localizedDescription)", "Unable to complete uninstall: \(error.localizedDescription)")) }
    }
    func applicationWillTerminate(_ notification: Notification) {
        if engine != nil { engine.stop() }
    }
}

@main struct PassportMain {
    static func mainMenu() -> NSMenu {
        let menu = NSMenu()
        let applicationItem = NSMenuItem()
        applicationItem.submenu = NSMenu(title: "Passport Sync")
        let preferences = NSMenuItem(title: appText("应用设置…", "App Settings…"),
                                     action: #selector(ManagementApp.showAppSettings), keyEquivalent: ",")
        preferences.target = NSApp.delegate
        applicationItem.submenu?.addItem(preferences)
        applicationItem.submenu?.addItem(.separator())
        applicationItem.submenu?.addItem(withTitle: appText("退出 Passport Sync", "Quit Passport Sync"), action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        menu.addItem(applicationItem)

        // AppKit dispatches editing shortcuts through the menu to the first responder, including modal fields.
        let edit = NSMenu(title: appText("编辑", "Edit"))
        edit.addItem(withTitle: appText("剪切", "Cut"), action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: appText("复制", "Copy"), action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: appText("粘贴", "Paste"), action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: appText("全选", "Select All"), action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        let editItem = NSMenuItem()
        editItem.submenu = edit
        menu.addItem(editItem)
        return menu
    }

    static func main() throws {
        let args = CommandLine.arguments
        if args.contains("--self-test") { testAppLanguage(); try testQuotaData(); return }
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
        app.setActivationPolicy(.accessory); app.delegate = delegate; app.mainMenu = mainMenu()
        withExtendedLifetime(delegate) { app.run() }
    }
}
