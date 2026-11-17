import Foundation
import Darwin

enum ReadError: LocalizedError {
    case protocolError, unavailable, cancelled
    var errorDescription: String? {
        switch self {
        case .protocolError: return appText("返回数据格式不符合协议", "The response does not match the protocol")
        case .unavailable: return appText("账号额度暂不可用，请检查 Codex 登录与网络", "Quota is unavailable. Check your Codex sign-in and connection.")
        case .cancelled: return appText("查询已取消", "Query canceled")
        }
    }
}

final class QueryCancellation {
    private let lock = NSLock()
    private var cancelled = false
    private var process: Process?
    func start(_ process: Process) throws {
        lock.lock(); defer { lock.unlock() }
        if cancelled { throw ReadError.cancelled }
        try process.run(); self.process = process
    }
    func cancel(wait: Bool = false) {
        lock.lock()
        cancelled = true
        let child = process
        lock.unlock()
        if let child, child.isRunning {
            child.terminate()
            if wait {
                let killWork = DispatchWorkItem { if child.isRunning { _ = kill(child.processIdentifier, SIGKILL) } }
                DispatchQueue.global().asyncAfter(deadline: .now() + 1, execute: killWork)
                child.waitUntilExit(); killWork.cancel()
            }
        }
    }
}

func readLimits(codex: String, cancellation: QueryCancellation = QueryCancellation()) throws -> QuotaData {
    let process = Process()
    let input = Pipe()
    let output = Pipe()
    process.executableURL = URL(fileURLWithPath: codex)
    process.arguments = ["app-server", "--listen", "stdio://"]
    var environment = ProcessInfo.processInfo.environment
    let codexDirectory = URL(fileURLWithPath: codex).deletingLastPathComponent().path
    environment["PATH"] = codexDirectory + ":" + (environment["PATH"] ?? "/usr/bin:/bin")
    process.environment = environment
    process.standardInput = input
    process.standardOutput = output
    process.standardError = FileHandle.nullDevice
    try cancellation.start(process)
    let child = process.processIdentifier
    let timeout = DispatchWorkItem {
        if process.isRunning { _ = kill(child, SIGKILL) }
    }
    DispatchQueue.global().asyncAfter(deadline: .now() + 18, execute: timeout)
    defer {
        try? input.fileHandleForWriting.close()
        if process.isRunning { process.terminate() }
        process.waitUntilExit()
        timeout.cancel()
    }
    func send(_ message: [String: Any]) throws {
        let data = try JSONSerialization.data(withJSONObject: message)
        try input.fileHandleForWriting.write(contentsOf: data + Data([10]))
    }
    var pending = Data()
    func reply(_ id: Int) throws -> [String: Any] {
        while pending.count < 512 * 1024 {
            if let newline = pending.firstIndex(of: 10) {
                let line = pending.prefix(upTo: newline)
                pending = Data(pending.suffix(from: pending.index(after: newline)))
                guard let item = (try? JSONSerialization.jsonObject(with: line)) as? [String: Any],
                      item["id"] as? Int == id else { continue }
                guard let result = item["result"] as? [String: Any] else { throw ReadError.unavailable }
                return result
            }
            let chunk = output.fileHandleForReading.availableData
            if chunk.isEmpty { break }
            pending.append(chunk)
        }
        throw ReadError.protocolError
    }
    try send(["id": 1, "method": "initialize",
              "params": ["clientInfo": ["name": "folotoy-passport", "version": "1.0.0"]]])
    _ = try reply(1)
    try send(["method": "initialized"])
    try send(["id": 2, "method": "account/rateLimits/read"])
    return try QuotaData.decode(JSONSerialization.data(withJSONObject: reply(2)))
}

