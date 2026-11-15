import AppKit

enum PassportUI {
    static let background = NSColor(srgbRed: 0.04, green: 0.045, blue: 0.05, alpha: 1)
    static let surface = NSColor(srgbRed: 0.085, green: 0.10, blue: 0.105, alpha: 1)
    static let border = NSColor(white: 0.17, alpha: 1)
    static let danger = NSColor(srgbRed: 1, green: 0.42, blue: 0.45, alpha: 1)

    static func style(_ window: NSWindow) {
        window.titlebarAppearsTransparent = true
        window.appearance = NSAppearance(named: .darkAqua)
        window.backgroundColor = background
    }

    static func label(_ title: String, size: CGFloat = 13, color: NSColor = .secondaryLabelColor,
                      weight: NSFont.Weight = .regular, wraps: Bool = false) -> NSTextField {
        let view = wraps ? NSTextField(wrappingLabelWithString: title) : NSTextField(labelWithString: title)
        view.font = .systemFont(ofSize: size, weight: weight)
        view.textColor = color
        return view
    }

    static func button(_ title: String, _ target: AnyObject, _ action: Selector,
                       primary: Bool = false, destructive: Bool = false) -> NSButton {
        let button = NSButton(title: title, target: target, action: action)
        button.bezelStyle = .rounded
        button.isBordered = false
        button.wantsLayer = true
        button.layer?.cornerRadius = 10
        button.layer?.backgroundColor = (primary ? accent : destructive ? danger : NSColor(white: 0.16, alpha: 1)).cgColor
        button.contentTintColor = primary || destructive ? .black : .labelColor
        button.font = .systemFont(ofSize: 13, weight: .medium)
        button.heightAnchor.constraint(equalToConstant: 36).isActive = true
        button.widthAnchor.constraint(greaterThanOrEqualToConstant: 94).isActive = true
        return button
    }

    static func row(_ views: [NSView], spacing: CGFloat = 12) -> NSStackView {
        let stack = NSStackView(views: views)
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.spacing = spacing
        return stack
    }

    static func column(_ views: [NSView], spacing: CGFloat = 10) -> NSStackView {
        let stack = NSStackView(views: views)
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = spacing
        return stack
    }

    static func card(_ views: [NSView], inset: CGFloat = 18) -> NSStackView {
        let stack = column(views, spacing: 14)
        stack.edgeInsets = NSEdgeInsets(top: inset, left: inset, bottom: inset, right: inset)
        stack.wantsLayer = true
        stack.layer?.backgroundColor = surface.cgColor
        stack.layer?.cornerRadius = 16
        stack.layer?.borderWidth = 1
        stack.layer?.borderColor = border.cgColor
        return stack
    }

    static func fill(_ view: NSView, in parent: NSView, inset: CGFloat) {
        view.translatesAutoresizingMaskIntoConstraints = false
        parent.addSubview(view)
        NSLayoutConstraint.activate([
            view.leadingAnchor.constraint(equalTo: parent.leadingAnchor, constant: inset),
            view.trailingAnchor.constraint(equalTo: parent.trailingAnchor, constant: -inset),
            view.topAnchor.constraint(equalTo: parent.topAnchor, constant: inset),
            view.bottomAnchor.constraint(equalTo: parent.bottomAnchor, constant: -inset)
        ])
    }
}

final class PassportDialog: NSObject, NSWindowDelegate {
    let panel: NSPanel

    init(title: String, detail: String, content: NSView? = nil, confirm: String = "完成",
         cancel: String? = nil, destructive: Bool = false) {
        panel = NSPanel(contentRect: .zero, styleMask: [.titled, .closable], backing: .buffered, defer: false)
        super.init()
        panel.title = title
        panel.isReleasedWhenClosed = false
        panel.delegate = self
        PassportUI.style(panel)
        let heading = PassportUI.label(title, size: 20, color: .labelColor, weight: .semibold)
        let description = PassportUI.label(detail, wraps: true)
        let contents = PassportUI.card([description] + (content.map { [$0] } ?? []))
        description.widthAnchor.constraint(equalTo: contents.widthAnchor, constant: -36).isActive = true
        if let content { content.widthAnchor.constraint(equalTo: description.widthAnchor).isActive = true }
        var buttons = [NSView()]
        if let cancel {
            let dismiss = PassportUI.button(cancel, self, #selector(cancelDialog))
            dismiss.keyEquivalent = "\u{1b}"
            buttons.append(dismiss)
        }
        let proceed = PassportUI.button(confirm, self, #selector(confirmDialog), primary: !destructive, destructive: destructive)
        if !destructive { proceed.keyEquivalent = "\r" }
        buttons.append(proceed)
        let actions = PassportUI.row(buttons)
        let stack = PassportUI.column([heading, contents, actions], spacing: 16)
        contents.widthAnchor.constraint(equalToConstant: 424).isActive = true
        actions.widthAnchor.constraint(equalTo: contents.widthAnchor).isActive = true
        PassportUI.fill(stack, in: panel.contentView!, inset: 20)
        let height = stack.fittingSize.height + 40
        panel.setContentSize(NSSize(width: 464, height: height))
        panel.initialFirstResponder = content as? NSTextField
        panel.center()
    }

    func run() -> Bool {
        let result = NSApp.runModal(for: panel)
        panel.orderOut(nil)
        return result == .OK
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        cancelDialog()
        return false
    }

    @objc private func confirmDialog() { NSApp.stopModal(withCode: .OK) }
    @objc private func cancelDialog() { NSApp.stopModal(withCode: .cancel) }
}
