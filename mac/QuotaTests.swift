import Foundation
import CoreBluetooth

func testQuotaData() throws {
    let raw = Data(#"{"rateLimitsByLimitId":{"codex":{"primary":{"windowDurationMins":300,"usedPercent":25,"resetsAt":1700000001}}},"rateLimitResetCredits":{"availableCount":3,"credits":[{"status":"available","expiresAt":1796068200},{"status":"available","expiresAt":1796163892},{"status":"available","expiresAt":1797114020}]}}"#.utf8)
    let value = try QuotaData.decode(raw)
    let date = Date(timeIntervalSince1970: 1700000000)
    precondition(value.resets?.availableCount == 3 && value.resets?.complete == true)
    precondition(value.resets?.nextExpiry == 1796068200)
    precondition(Array(quotaPacket(at: date, offset: 480, windows: value.windows)) == [0,241,83,101,224,1,44,1,75,1,241,83,101,0,0,0,0,0,0,0])
    let packet = resetPacket(at: date, resets: value.resets)
    precondition(Array(packet) == [1,7,3,0,104,211,13,107,0,241,83,101])
    let unknown = try QuotaData.decode(Data(#"{"rateLimitResetCredits":null}"#.utf8))
    precondition(unknown.resets == nil && resetPacket(at: date, resets: nil)[1] == 0)
    let zero = try QuotaData.decode(Data(#"{"rateLimitResetCredits":{"availableCount":0,"credits":[]}}"#.utf8))
    precondition(zero.resets?.availableCount == 0 && resetPacket(at: date, resets: zero.resets)[1] == 5)
    let capped = try QuotaData.decode(Data(#"{"rateLimitResetCredits":{"availableCount":4,"credits":[{"status":"available","expiresAt":1796068200}]}}"#.utf8))
    precondition(capped.resets?.availableCount == 4 && capped.resets?.complete == false)
    let noDates = try QuotaData.decode(Data(#"{"rateLimitResetCredits":{"availableCount":2,"credits":null}}"#.utf8))
    precondition(noDates.resets?.nextExpiry == nil && noDates.resets?.complete == false)
    for json in [#"{"rateLimitResetCredits":{"availableCount":-1}}"#,
                 #"{"rateLimitResetCredits":{"availableCount":65536}}"#,
                 #"{"rateLimits":{"primary":{"windowDurationMins":300,"usedPercent":101}}}"#] {
        do { _ = try QuotaData.decode(Data(json.utf8)); fatalError("Malformed external data accepted") }
        catch { }
    }
    precondition(resetMessageUnsupported(NSError(domain: CBATTErrorDomain, code: CBATTError.invalidAttributeValueLength.rawValue)))
    precondition(!resetMessageUnsupported(NSError(domain: CBATTErrorDomain, code: CBATTError.insufficientAuthentication.rawValue)))
    precondition(!resetMessageUnsupported(NSError(domain: "transport", code: CBATTError.invalidAttributeValueLength.rawValue)))
    precondition(combinedTaskCount(local: 2, remote: 3) == 5)
    precondition(combinedTaskCount(local: 2, remote: nil) == 2)
    precondition(combinedTaskCount(local: nil, remote: 0) == 0)
    precondition(combinedTaskCount(local: nil, remote: nil) == nil)
    precondition(combinedTaskCount(local: 250, remote: 10) == 255)
    precondition(isValidSSHHost("dev_box"))
    precondition(isValidSSHHost("codex.example.net"))
    precondition(!isValidSSHHost("-oProxyCommand"))
    precondition(!isValidSSHHost("user@host"))
    precondition(!isValidSSHHost("host\n"))
    precondition(!isValidSSHHost(""))
    var remoteCompletion = RemoteCompletionCursor()
    precondition(!remoteCompletion.consume(RemoteTaskSnapshot(active: 1, observedAt: 10, latestCompletionAt: 8)))
    precondition(remoteCompletion.consume(RemoteTaskSnapshot(active: 0, observedAt: 20, latestCompletionAt: 17)))
    precondition(!remoteCompletion.consume(RemoteTaskSnapshot(active: 0, observedAt: 30, latestCompletionAt: 17)))
    precondition(!remoteCompletion.consume(nil))
    precondition(!remoteCompletion.consume(RemoteTaskSnapshot(active: 0, observedAt: 40, latestCompletionAt: 38)))
    precondition(!remoteCompletion.consume(RemoteTaskSnapshot(active: 0, observedAt: 50, latestCompletionAt: 38)))
    precondition(remoteCompletion.consume(RemoteTaskSnapshot(active: 0, observedAt: 60, latestCompletionAt: 55)))
    try testTaskJournal()
    print("Quota checks passed: windows, reset count, unknown/zero, partial details, bounds and BLE packets.")
}


private func testTaskJournal() throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("passport-task-tests-" + UUID().uuidString)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let first = root.appendingPathComponent("one.jsonl"), second = root.appendingPathComponent("two.jsonl")
    let child = root.appendingPathComponent("child.jsonl")
    let start = Date()
    func writeSource(_ file: URL, _ source: Any) throws {
        let meta = ["type": "session_meta", "payload": ["source": source]] as [String: Any]
        try (JSONSerialization.data(withJSONObject: meta) + Data([10])).write(to: file)
    }
    func append(_ file: URL, _ kind: String, _ turn: String, root: String? = nil, finished: Date? = nil) throws {
        var event: [String: Any] = ["type":kind, "turn_id":turn, "root_turn_id":root ?? turn]
        if let finished { event["completed_at"] = Int(finished.timeIntervalSince1970) }
        let line = try JSONSerialization.data(withJSONObject: ["type":"event_msg", "payload":event]) + Data([10])
        if !FileManager.default.fileExists(atPath: file.path) { try line.write(to: file); return }
        let stream = try FileHandle(forWritingTo: file)
        defer { try? stream.close() }
        try stream.seekToEnd()
        try stream.write(contentsOf: line)
    }
    try writeSource(first, "vscode")
    try writeSource(child, ["subagent": "review"])
    try append(child, "task_started", "child")
    try append(first, "task_started", "historic")
    try append(first, "task_complete", "historic", finished: start)
    try append(first, "task_started", "a", root: "another-session-turn")
    // 模拟附件和工具输出把开始事件推离最后 512 KiB。
    let output = try FileHandle(forWritingTo: first)
    try output.seekToEnd()
    try output.write(contentsOf: Data(repeating: 120, count: 700_000) + Data([10]))
    try output.close()
    try append(second, "task_started", "b")
    try append(second, "task_started", "subagent", root: "b")
    let journal = CodexTaskJournal(root: root)
    precondition(journal.scan(now: start) == CodexTaskStatus(active: 2))
    let secondPoll = start.addingTimeInterval(3)
    try append(first, "task_complete", "a", root: "another-session-turn", finished: secondPoll)
    precondition(journal.scan(now: secondPoll) == CodexTaskStatus(active: 1, completed: true))
    try append(second, "turn_aborted", "b")
    try append(second, "task_complete", "b", finished: secondPoll)
    precondition(journal.scan(now: secondPoll.addingTimeInterval(3)) == CodexTaskStatus(active: 0))
    precondition(journal.scan(now: secondPoll.addingTimeInterval(4), baseline: true) == CodexTaskStatus(active: 0))
    precondition(Array(syncStatusPacket(active: 2, quotaFailed: true, completed: true)) == [1,7,2])
    precondition(Array(syncStatusPacket(active: nil, quotaFailed: true, completed: true)) == [1,1,0])
    print("Task status checks passed: parallel and cross-session tasks, subagent filtering, completion, abort and reconnect baseline.")
}
