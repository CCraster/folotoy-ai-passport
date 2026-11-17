import Foundation

enum AppLanguage: String, CaseIterable {
    case system, chinese, english

    private static let preferenceKey = "appLanguage"

    static func saved(in defaults: UserDefaults = .standard) -> AppLanguage {
        defaults.string(forKey: preferenceKey).flatMap(AppLanguage.init(rawValue:)) ?? .system
    }

    func save(in defaults: UserDefaults = .standard) {
        defaults.set(rawValue, forKey: Self.preferenceKey)
    }

    func locale(preferred: [String] = Locale.preferredLanguages) -> String {
        switch self {
        case .chinese: return "zh-CN"
        case .english: return "en"
        case .system: return preferred.first?.hasPrefix("zh") == true ? "zh-CN" : "en"
        }
    }

    var title: String {
        switch self {
        case .system: return appText("跟随系统", "Follow System")
        case .chinese: return "简体中文"
        case .english: return "English"
        }
    }
}

func appText(_ chinese: String, _ english: String) -> String {
    AppLanguage.saved().locale() == "en" ? english : chinese
}


func testAppLanguage() {
    let suite = "PassportLanguageTest." + UUID().uuidString
    let defaults = UserDefaults(suiteName: suite)!
    defer { defaults.removePersistentDomain(forName: suite) }
    precondition(AppLanguage.saved(in: defaults) == .system)
    precondition(AppLanguage.system.locale(preferred: ["zh-Hans"]) == "zh-CN")
    precondition(AppLanguage.system.locale(preferred: ["en-US"]) == "en")
    for language in [AppLanguage.chinese, .english, .system] {
        language.save(in: defaults)
        precondition(AppLanguage.saved(in: UserDefaults(suiteName: suite)!) == language)
    }
    print("App language checks passed: system default, Chinese and English persistence.")
}
