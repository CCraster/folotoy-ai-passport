import Foundation

struct QuotaWindow {
    var minutes: UInt16 = 0
    var remaining: UInt8 = 0
    var resetAt: UInt32 = 0
}

struct ResetCredit: Decodable {
    let status: String
    let expiresAt: UInt32?
}

struct ResetCredits: Decodable {
    let availableCount: UInt16
    let credits: [ResetCredit]?

    // 服务端可能只返回部分详情；张数始终采用 availableCount。
    var complete: Bool { credits.map { $0.count == Int(availableCount) } ?? (availableCount == 0) }
    var nextExpiry: UInt32? { credits?.filter { $0.status == "available" }.compactMap(\.expiresAt).min() }
}

struct QuotaData {
    let windows: [QuotaWindow]
    let resets: ResetCredits?

    static func decode(_ data: Data) throws -> QuotaData {
        struct RawWindow: Decodable { let windowDurationMins: UInt16; let usedPercent: Double; let resetsAt: UInt32? }
        struct Bucket: Decodable { let primary: RawWindow?; let secondary: RawWindow?; let limitId: String? }
        struct Response: Decodable {
            let rateLimitsByLimitId: [String: Bucket]?
            let rateLimits: Bucket?
            let rateLimitResetCredits: ResetCredits?
        }
        let raw = try JSONDecoder().decode(Response.self, from: data)
        let bucket = raw.rateLimitsByLimitId?["codex"] ??
            (raw.rateLimits?.limitId == nil || raw.rateLimits?.limitId == "codex" ? raw.rateLimits : nil)
        let windows = try [bucket?.primary, bucket?.secondary].map { value -> QuotaWindow in
            guard let value else { return QuotaWindow() }
            guard value.windowDurationMins > 0, value.usedPercent.isFinite,
                  (0...100).contains(value.usedPercent) else { throw ReadError.protocolError }
            return QuotaWindow(minutes: value.windowDurationMins,
                               remaining: UInt8((100 - value.usedPercent).rounded()), resetAt: value.resetsAt ?? 0)
        }
        return QuotaData(windows: windows, resets: raw.rateLimitResetCredits)
    }
}

func quotaPacket(at date: Date, offset: Int, windows: [QuotaWindow]) -> Data {
    var bytes = Data()
    func append<T: FixedWidthInteger>(_ value: T) {
        var little = value.littleEndian
        withUnsafeBytes(of: &little) { bytes.append(contentsOf: $0) }
    }
    append(UInt32(date.timeIntervalSince1970)); append(Int16(offset))
    for window in windows { append(window.minutes); append(window.remaining); append(window.resetAt) }
    return bytes
}

// 独立的 12 字节特征兼容旧固件，也适配默认 ATT MTU；未知与零张具有不同标志。
func resetPacket(at date: Date, resets: ResetCredits?) -> Data {
    var flags: UInt8 = resets == nil ? 0 : 1
    if resets?.nextExpiry != nil { flags |= 2 }
    if resets?.complete == true { flags |= 4 }
    var bytes = Data([1, flags])
    func append<T: FixedWidthInteger>(_ value: T) {
        var little = value.littleEndian
        withUnsafeBytes(of: &little) { bytes.append(contentsOf: $0) }
    }
    append(resets?.availableCount ?? 0); append(resets?.nextExpiry ?? 0)
    append(UInt32(date.timeIntervalSince1970))
    return bytes
}
