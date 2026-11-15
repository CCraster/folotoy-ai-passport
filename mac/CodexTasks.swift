import Foundation

struct CodexTaskStatus: Equatable {
    var active: UInt8?
    var completed = false
}

func syncStatusPacket(active: UInt8?, quotaFailed: Bool, completed: Bool) -> Data {
    let flags: UInt8 = (quotaFailed ? 1 : 0) | (active == nil ? 0 : 2) | (active != nil && completed ? 4 : 0)
    return Data([1, flags, active ?? 0])
}

final class CodexTaskJournal {
    private struct Event: Decodable {
        struct Payload: Decodable {
            let type: String
            let turnId: String?
            let rootTurnId: String?
            let completedAt: Int?
        }
        let type: String
        let payload: Payload
    }

    private struct Session {
        var cursor: UInt64 = 0
        var active: Set<String> = []
    }

    private static let lifecycleTags = ["task_started", "task_complete", "turn_aborted"]
        .map { Data(("\"" + $0 + "\"").utf8) }
    private let root: URL
    private var sessions: [URL: Session] = [:]
    private var previousScan: Date?
    private let decoder: JSONDecoder

    init(root: URL? = nil) {
        let home = ProcessInfo.processInfo.environment["CODEX_HOME"].map { URL(fileURLWithPath: $0) }
            ?? FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".codex")
        self.root = root ?? home.appendingPathComponent("sessions")
        decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
    }

    func scan(now: Date = Date(), baseline: Bool = false) -> CodexTaskStatus {
        if baseline { sessions.removeAll(); previousScan = nil }
        // 最近写入的会话来自同一台 Mac。只消费生命周期记录，旧完成事件用于建立基线。
        let keys: Set<URLResourceKey> = [.contentModificationDateKey, .fileSizeKey, .isRegularFileKey]
        var candidates: [(URL, Date, Int)] = []
        var scanFailed = false
        guard let files = FileManager.default.enumerator(
            at: root, includingPropertiesForKeys: Array(keys),
            options: [.skipsHiddenFiles, .skipsPackageDescendants],
            errorHandler: { _, _ in scanFailed = true; return false }
        ) else { return CodexTaskStatus(active: nil) }
        do {
            for case let file as URL in files where file.pathExtension == "jsonl" {
                let attrs = try file.resourceValues(forKeys: keys)
                guard attrs.isRegularFile == true, let updated = attrs.contentModificationDate,
                      now.timeIntervalSince(updated) < 7200, let size = attrs.fileSize else { continue }
                candidates.append((file, updated, size))
            }
        } catch { return CodexTaskStatus(active: nil) }
        if scanFailed { return CodexTaskStatus(active: nil) }

        var next: [URL: Session] = [:]
        var completed = false
        do {
            for (file, _, bytes) in candidates.sorted(by: { $0.1 > $1.1 }).prefix(64) {
                var state = sessions[file] ?? Session()
                let size = UInt64(bytes)
                if size < state.cursor { state = Session() }
                // 首次连接时长任务的开始事件可能已经隔了许多输出和附件。
                // 用较长的尾段建立基线，随后只处理追加内容。
                let limit: UInt64 = state.cursor == 0 ? 8 * 1024 * 1024 : 512 * 1024
                let floor = size > limit ? size - limit : 0
                let start = max(floor, state.cursor)
                if start != state.cursor { state.active.removeAll() }
                if start < size {
                    let handle = try FileHandle(forReadingFrom: file)
                    defer { try? handle.close() }
                    try handle.seek(toOffset: start)
                    if let chunk = try handle.read(upToCount: Int(size - start)) {
                        var lineStart = chunk.startIndex
                        var skipFirst = start != state.cursor
                        // 光标停在最后一个换行，下一次可以继续消费尚未写完的记录。
                        for lineEnd in chunk.indices where chunk[lineEnd] == 10 {
                            defer { lineStart = lineEnd + 1 }
                            guard !skipFirst else { skipFirst = false; continue }
                            let line = chunk[lineStart..<lineEnd]
                            guard Self.lifecycleTags.contains(where: { line.range(of: $0) != nil }),
                                  let event = try? decoder.decode(Event.self, from: line),
                                  event.type == "event_msg", let turn = event.payload.turnId,
                                  event.payload.rootTurnId == nil || event.payload.rootTurnId == turn else { continue }
                            switch event.payload.type {
                            case "task_started": state.active.insert(turn)
                            case "task_complete":
                                let seen = state.active.remove(turn) != nil
                                if seen, let previousScan, let finished = event.payload.completedAt,
                                   finished >= Int(previousScan.timeIntervalSince1970),
                                   finished <= Int(now.timeIntervalSince1970) {
                                    completed = true
                                }
                            case "turn_aborted": state.active.remove(turn)
                            default: break
                            }
                        }
                        state.cursor = start + UInt64(lineStart)
                    }
                }
                next[file] = state
            }
        } catch {
            return CodexTaskStatus(active: nil)
        }
        sessions = next
        previousScan = now
        let count = sessions.values.reduce(0) { $0 + $1.active.count }
        return CodexTaskStatus(active: UInt8(clamping: count), completed: completed)
    }
}
