import Foundation
import Darwin

struct RemoteTaskSnapshot: Decodable {
    let active: UInt8
    let observedAt: TimeInterval
    let latestCompletionAt: TimeInterval?
}

struct RemoteCompletionCursor {
    private var lastCompletionAt: TimeInterval?

    mutating func consume(_ snapshot: RemoteTaskSnapshot?) -> Bool {
        guard let snapshot else {
            lastCompletionAt = nil
            return false
        }
        guard let lastCompletionAt else {
            // Baseline from the remote clock so an old completion is never replayed on connect.
            self.lastCompletionAt = max(snapshot.observedAt, snapshot.latestCompletionAt ?? 0)
            return false
        }
        guard let completedAt = snapshot.latestCompletionAt, completedAt > lastCompletionAt else { return false }
        self.lastCompletionAt = completedAt
        return true
    }
}

// Only accept a host or SSH config alias. User, port and jump host belong in ~/.ssh/config.
func isValidSSHHost(_ host: String) -> Bool {
    host.utf8.count <= 253 && host.range(of: #"\A[a-zA-Z0-9_][a-zA-Z0-9_.-]*\z"#, options: .regularExpression) != nil
}

func readRemoteTaskSnapshot(host: String, cancellation: QueryCancellation) throws -> RemoteTaskSnapshot {
    guard let script = Bundle.main.url(forResource: "RemoteTaskCount", withExtension: "py") else {
        throw ReadError.protocolError
    }
    let process = Process()
    let output = Pipe()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/ssh")
    process.arguments = ["-T", "-a",
        "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o", "UpdateHostKeys=no",
        "-o", "ControlMaster=no", "-o", "ControlPath=none", "-o", "ClearAllForwardings=yes",
        "-o", "PermitLocalCommand=no", "-o", "ConnectTimeout=5", "-o", "ConnectionAttempts=1",
        "-o", "LogLevel=ERROR", "--", host, "python3 -"]
    let input = try FileHandle(forReadingFrom: script)
    defer { try? input.close() }
    process.standardInput = input
    process.standardOutput = output
    process.standardError = FileHandle.nullDevice
    try cancellation.start(process)
    let timeout = DispatchWorkItem {
        if process.isRunning { _ = kill(process.processIdentifier, SIGKILL) }
    }
    DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 8, execute: timeout)
    defer {
        if process.isRunning { process.terminate() }
        process.waitUntilExit()
        timeout.cancel()
    }
    // 只接受一个有界 JSON 响应，远端 shell 横幅和其他输出会使本轮降级。
    var data = Data()
    while data.count < 1024 {
        let chunk = try output.fileHandleForReading.read(upToCount: 1024 - data.count) ?? Data()
        if chunk.isEmpty { break }
        data.append(chunk)
    }
    guard data.count < 1024 else { throw ReadError.protocolError }
    process.waitUntilExit()
    guard process.terminationStatus == 0 else { throw ReadError.unavailable }
    return try JSONDecoder().decode(RemoteTaskSnapshot.self, from: data)
}

func combinedTaskCount(local: UInt8?, remote: UInt8?) -> UInt8? {
    guard local != nil || remote != nil else { return nil }
    return UInt8(clamping: Int(local ?? 0) + Int(remote ?? 0))
}
