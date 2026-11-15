import CoreBluetooth
import Foundation

let serviceID = CBUUID(string: "9B008321-4D45-49F7-A5B9-5C317A34CE81")
let dataID = CBUUID(string: "9B008322-4D45-49F7-A5B9-5C317A34CE81")
// 旧固件以 ATT 长度错误拒绝新消息，此时保留额度同步并提示升级。
func resetMessageUnsupported(_ error: Error) -> Bool {
    let value = error as NSError
    return value.domain == CBATTErrorDomain && value.code == CBATTError.invalidAttributeValueLength.rawValue
}

private struct SyncWrite {
    enum Kind { case clock, quota, resets, status }
    let kind: Kind
    let data: Data
}

final class PassportSync: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private let codex: String
    private var central: CBCentralManager!
    private var device: CBPeripheral?
    private var characteristic: CBCharacteristic?
    private var resetsUnsupported = false
    private var statusUnsupported = false
    private var quotaFailed = false
    private var taskTimer: Timer?
    private var remoteTimer: Timer?
    private var remoteQuery: QueryCancellation?
    private var remoteCompletion = RemoteCompletionCursor()
    private(set) var remoteHost = UserDefaults.standard.string(forKey: "remoteSSHHost").flatMap {
        isValidSSHHost($0) ? $0 : nil
    }
    private(set) var localTasks: UInt8?
    private(set) var remoteTasks: UInt8?
    private(set) var remoteUnavailable = false
    private(set) var acknowledgedTasks: UInt8?
    private(set) var lastCompletionAcknowledged: Date?
    private let taskQueue = DispatchQueue(label: "passport.codex-tasks", qos: .utility)
    private let journal = CodexTaskJournal()
    private var journalGeneration = -1
    private var taskQuerying = false
    private var lastPublishedStatus: Data?
    var activeTasks: UInt8? { combinedTaskCount(local: localTasks, remote: remoteTasks) }
    private var querying = false
    private var queryCancellation: QueryCancellation?
    private var timer: Timer?
    private var firstWrite = true
    private var writes: [SyncWrite] = []
    private var writeInFlight = false
    private var generation = 0
    private var writeSequence = 0
    private(set) var connected = false
    private(set) var paused = UserDefaults.standard.bool(forKey: "syncPaused")
    private(set) var connection = "正在启动"
    var connectionHint: String? {
        guard !paused else { return nil }
        switch connection {
        case "蓝牙未授权": return "在 Mac 的「系统设置 → 隐私与安全性 → 蓝牙」中允许 Passport Sync。"
        case "蓝牙已关闭": return "打开 Mac 蓝牙，再确认卡片的蓝牙开关已开启。"
        case "等待设备": return "在卡片长按确认，进入「连接 → Mac 蓝牙 → 配对 Mac」。"
        case "正在验证配对": return "Mac 请求配对码时，输入卡片屏幕上的六位数字。"
        default: return nil
        }
    }
    private(set) var error: String?
    private(set) var lastRead: Date?
    private(set) var lastSync: Date?
    private(set) var latest: QuotaData?
    var changed: (() -> Void)?
    private var selectedDevice: UUID? {
        UserDefaults.standard.string(forKey: "passportDevice").flatMap(UUID.init(uuidString:))
    }

    init(codex: String) {
        self.codex = codex
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
        timer = Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { [weak self] _ in self?.refresh() }
        taskTimer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in self?.refreshTasks() }
        scheduleRemotePoll()
    }

    func setRemoteHost(_ host: String?) {
        guard remoteHost != host else { return }
        remoteHost = host
        UserDefaults.standard.set(host, forKey: "remoteSSHHost")
        remoteQuery?.cancel()
        remoteQuery = nil
        remoteCompletion = RemoteCompletionCursor()
        remoteTasks = nil
        remoteUnavailable = false
        scheduleRemotePoll()
        publishStatus()
        changed?()
        refreshRemoteTasks()
    }

    private func scheduleRemotePoll() {
        remoteTimer?.invalidate()
        remoteTimer = nil
        guard remoteHost != nil else { return }
        remoteTimer = Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in self?.refreshRemoteTasks() }
    }

    func setPaused(_ value: Bool) {
        paused = value; UserDefaults.standard.set(value, forKey: "syncPaused"); generation += 1
        if value {
            queryCancellation?.cancel()
            central.stopScan()
            if let device { central.cancelPeripheralConnection(device) }
            clearConnection(); connection = "已暂停"
        } else { scan() }
        changed?()
    }
    func stop() {
        queryCancellation?.cancel(wait: true); remoteQuery?.cancel(wait: true)
        timer?.invalidate(); taskTimer?.invalidate(); remoteTimer?.invalidate(); central.stopScan()
        if let device { central.cancelPeripheralConnection(device) }
    }
    private func clearConnection() {
        device = nil; characteristic = nil; resetsUnsupported = false
        remoteQuery?.cancel()
        remoteQuery = nil
        remoteCompletion = RemoteCompletionCursor()
        quotaFailed = false; localTasks = nil; remoteTasks = nil; remoteUnavailable = false
        acknowledgedTasks = nil; statusUnsupported = false
        lastPublishedStatus = nil
        writes.removeAll(); writeInFlight = false; connected = false; firstWrite = true
    }
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn { scan() }
        else {
            generation += 1; clearConnection()
            switch central.state {
            case .unauthorized: connection = "蓝牙未授权"
            case .poweredOff: connection = "蓝牙已关闭"
            case .unsupported: connection = "蓝牙不可用"
            default: connection = "蓝牙准备中"
            }
            changed?()
        }
    }
    private func scan() {
        guard !paused, central.state == .poweredOn, device == nil else { return }
        connection = "等待设备"; central.scanForPeripherals(withServices: [serviceID]); changed?()
    }
    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard !paused, device == nil, selectedDevice == nil || selectedDevice == peripheral.identifier else { return }
        central.stopScan(); device = peripheral; peripheral.delegate = self
        connection = "正在连接"; changed?(); central.connect(peripheral)
        DispatchQueue.main.asyncAfter(deadline: .now() + 15) { [weak self, weak peripheral] in
            guard let self, let peripheral, self.device === peripheral, peripheral.state == .connecting else { return }
            self.central.cancelPeripheralConnection(peripheral)
        }
    }
    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([serviceID])
    }
    private func reconnect(_ peripheral: CBPeripheral, error: Error?) {
        guard device === peripheral else { return }
        generation += 1; clearConnection(); connection = paused ? "已暂停" : "等待设备"
        if let error { self.error = "蓝牙：\(error.localizedDescription)" }
        changed?()
        DispatchQueue.main.asyncAfter(deadline: .now() + 5) { [weak self] in self?.scan() }
    }
    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        reconnect(peripheral, error: error)
    }
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        reconnect(peripheral, error: error)
    }
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == serviceID }) else {
            self.error = "设备服务读取失败"; changed?(); central.cancelPeripheralConnection(peripheral); return
        }
        peripheral.discoverCharacteristics([dataID], for: service)
    }
    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        characteristic = service.characteristics?.first(where: { $0.uuid == dataID })
        guard characteristic != nil else { central.cancelPeripheralConnection(peripheral); return }
        connection = "正在验证配对"; changed?()
        enqueue([SyncWrite(kind: .clock, data: quotaPacket(at: Date(), offset: TimeZone.current.secondsFromGMT() / 60,
                                              windows: [QuotaWindow(), QuotaWindow()]))])
    }
    // 按消息逐个等待设备确认，整批完成后才记作一次成功同步。
    private func enqueue(_ packets: [SyncWrite]) {
        writes.append(contentsOf: packets); writeNext()
    }
    private func writeNext() {
        guard !writeInFlight, let device, let characteristic, let next = writes.first else { return }
        writeInFlight = true; writeSequence += 1
        let sequence = writeSequence
        device.writeValue(next.data, for: characteristic, type: .withResponse)
        DispatchQueue.main.asyncAfter(deadline: .now() + 125) { [weak self, weak device] in
            guard let self, let device, self.device === device,
                  self.writeSequence == sequence, self.writeInFlight else { return }
            self.error = "设备确认超时"; self.changed?(); self.central.cancelPeripheralConnection(device)
        }
    }
    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard device === peripheral, writeInFlight else { return }
        guard let write = writes.first else { return }
        if let error {
            if write.kind == .status && resetMessageUnsupported(error) {
                statusUnsupported = true
                // 老固件拒绝状态包，额度与时钟继续同步。
                writes.removeAll { $0.kind == .status }
                writeInFlight = false
                writeNext()
                return
            }
            if write.kind == .resets && resetMessageUnsupported(error) {
                resetsUnsupported = true
            } else {
                self.error = "同步失败：\(error.localizedDescription)"; changed?()
                central.cancelPeripheralConnection(peripheral); return
            }
        }
        writes.removeFirst(); writeInFlight = false
        if firstWrite {
            firstWrite = false; connected = true; connection = "已连接"; self.error = nil
            if selectedDevice == nil { UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: "passportDevice") }
            changed?(); refresh(); refreshTasks(); refreshRemoteTasks()
        } else if write.kind == .status {
            acknowledgedTasks = (write.data[1] & 2) != 0 ? write.data[2] : nil
            if (write.data[1] & 4) != 0 { lastCompletionAcknowledged = Date() }
            changed?()
        } else if write.kind == .resets || (write.kind == .quota && !writes.contains(where: { $0.kind == .resets })) {
            lastSync = Date(); self.error = resetsUnsupported ? "请更新卡片固件以显示重置卡" : nil; changed?()
        }
        writeNext()
    }
    private func publishStatus(completed: Bool = false) {
        guard connected, !statusUnsupported else { return }
        let latest = syncStatusPacket(active: activeTasks, quotaFailed: quotaFailed, completed: false)
        guard completed || latest != lastPublishedStatus else { return }
        lastPublishedStatus = latest
        enqueue([SyncWrite(kind: .status, data: completed ?
            syncStatusPacket(active: activeTasks, quotaFailed: quotaFailed, completed: true) : latest)])
    }

    private func refreshTasks() {
        guard connected, !paused, !taskQuerying, !writeInFlight, writes.isEmpty else { return }
        taskQuerying = true
        let epoch = generation
        taskQueue.async { [weak self] in
            guard let self else { return }
            let baseline = self.journalGeneration != epoch
            self.journalGeneration = epoch
            let state = self.journal.scan(baseline: baseline)
            DispatchQueue.main.async {
                self.taskQuerying = false
                guard self.connected, !self.paused, self.generation == epoch else { return }
                let changed = self.localTasks != state.active
                self.localTasks = state.active
                self.publishStatus(completed: state.completed)
                if changed { self.changed?() }
            }
        }
    }

    func refreshRemoteTasks() {
        guard let host = remoteHost, connected, !paused, remoteQuery == nil else { return }
        let cancellation = QueryCancellation()
        remoteQuery = cancellation
        let epoch = generation
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let snapshot = try? readRemoteTaskSnapshot(host: host, cancellation: cancellation)
            DispatchQueue.main.async {
                guard let self, self.remoteQuery === cancellation, self.generation == epoch else { return }
                self.remoteQuery = nil
                let count = snapshot?.active
                let changed = self.remoteTasks != count || self.remoteUnavailable != (count == nil)
                self.remoteTasks = count
                self.remoteUnavailable = count == nil
                let completed = self.remoteCompletion.consume(snapshot)
                self.publishStatus(completed: completed)
                if changed {
                    self.changed?()
                }
            }
        }
    }

    func refresh() {
        guard !paused, connected, characteristic != nil, !querying, !writeInFlight, writes.isEmpty else { return }
        querying = true
        let cancellation = QueryCancellation()
        queryCancellation = cancellation
        let codex = self.codex, epoch = generation
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let result = Result { try readLimits(codex: codex, cancellation: cancellation) }
            DispatchQueue.main.async {
                guard let self else { return }
                self.querying = false
                guard !self.paused, self.connected, self.generation == epoch else { return }
                switch result {
                case .success(let value):
                    let date = Date(); self.latest = value; self.lastRead = date
                    var packets = [SyncWrite(kind: .quota, data: quotaPacket(at: date,
                        offset: TimeZone.current.secondsFromGMT(for: date) / 60, windows: value.windows))]
                    if !self.resetsUnsupported { packets.append(SyncWrite(kind: .resets, data: resetPacket(at: date, resets: value.resets))) }
                    self.quotaFailed = !value.windows.contains { $0.minutes > 0 }
                    self.enqueue(packets)
                    self.publishStatus()
                case .failure(let error):
                    self.quotaFailed = true
                    self.error = "Codex 读取失败：\(error.localizedDescription)"
                    self.publishStatus()
                }
                self.changed?()
            }
        }
    }
}
