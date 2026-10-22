import SwiftUI

// The launcher's visual vocabulary: one card surface, one card header, one row shape, one set of buttons.
// Colours are semantic (primary/secondary/accent at fixed opacities) so light and dark both work.

private let hairline = Color.primary.opacity(0.07)
private let fieldFill = Color.primary.opacity(0.06)
private let fieldStroke = Color.primary.opacity(0.10)

/// A card surface: Liquid Glass (macOS 26) or a material, with a faint rim.
struct Surface: ViewModifier {
    var radius: CGFloat
    func body(content: Content) -> some View {
        content
            .glassPanel(cornerRadius: radius)
            .overlay(RoundedRectangle(cornerRadius: radius, style: .continuous).strokeBorder(Color.primary.opacity(0.08)))
    }
}

extension View {
    func surface(_ radius: CGFloat = 20) -> some View { modifier(Surface(radius: radius)) }

    /// One line inside a card: the card's insets and a hairline above it.
    func cardLine(vertical: CGFloat = 9) -> some View {
        self
            .padding(.leading, 16).padding(.trailing, 12).padding(.vertical, vertical)
            .frame(maxWidth: .infinity, alignment: .leading)
            .overlay(alignment: .top) { Rectangle().fill(hairline).frame(height: 1) }
    }
}

/// The small icon tile in front of a card title.
struct HeaderIcon: View {
    let systemName: String
    var body: some View {
        Image(systemName: systemName)
            .font(.system(size: 11, weight: .medium))
            .frame(width: 22, height: 22)
            .background(Color.primary.opacity(0.07), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
    }
}

extension EnvironmentValues {
    /// Cards drawn as grouped lines under a small heading (the create sheet) instead of titled glass cards.
    @Entry var groupedCards = false
}

/// The one type scale and control size every screen uses: titles are the only larger text, everything you read
/// or click is `body`, every explanation `caption`, and every button, menu, field and switch group is `control` tall.
enum Metrics {
    static let body: CGFloat = 12
    static let caption: CGFloat = 11
    static let control: CGFloat = 26
    /// Lines in the apps and scripts lists: a size smaller than `body` / `caption`, so the two-line rows breathe.
    static let listTitle: CGFloat = 11
    static let listDetail: CGFloat = 10
}

extension View {
    /// A small green dot on an icon's top-right corner: the app is in front, the script is running.
    func activeDot(_ on: Bool, help: String = "") -> some View {
        overlay(alignment: .topTrailing) {
            if on {
                Circle().fill(Color.green)
                    .frame(width: 8, height: 8)
                    .overlay(Circle().strokeBorder(Color.black.opacity(0.35), lineWidth: 1.5))
                    .offset(x: 3, y: -3)
                    .help(help)
            }
        }
    }
}

/// The SoulX mark: a phone on the blue-to-teal tile.
struct AppMark: View {
    var size: CGFloat = 30
    var body: some View {
        Image(systemName: "iphone.gen3")
            .font(.system(size: size * 0.43, weight: .semibold))
            .foregroundStyle(.white)
            .frame(width: size, height: size)
            .background(LinearGradient(colors: [Color(red: 0.30, green: 0.55, blue: 1.0),
                                                Color(red: 0.12, green: 0.71, blue: 0.63)],
                                       startPoint: .topLeading, endPoint: .bottomTrailing),
                        in: RoundedRectangle(cornerRadius: size / 3, style: .continuous))
    }
}

/// A titled card: icon, title, optional count, trailing actions, then its lines.
struct SectionCard<Trailing: View, Content: View>: View {
    let title: String
    let icon: String
    var count: Int?
    let trailing: () -> Trailing
    let content: () -> Content

    init(_ title: String, icon: String, count: Int? = nil,
         @ViewBuilder trailing: @escaping () -> Trailing, @ViewBuilder content: @escaping () -> Content) {
        self.title = title
        self.icon = icon
        self.count = count
        self.trailing = trailing
        self.content = content
    }

    @Environment(\.groupedCards) private var grouped

    var body: some View {
        if grouped { groupedBody } else { cardBody }
    }

    /// The settings-sheet look: a small heading over a plain group of lines.
    private var groupedBody: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.system(size: 11, weight: .semibold)).foregroundStyle(.secondary).padding(.leading, 4)
            VStack(spacing: 0) { content() }
                .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                .overlay(alignment: .top) { Color.primary.opacity(0.001).frame(height: 1) }
                .background(Color.primary.opacity(0.045), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(Color.primary.opacity(0.08)))
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var cardBody: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                HeaderIcon(systemName: icon)
                Text(title).font(.system(size: 13, weight: .semibold))
                if let count {
                    Text("\(count)").font(.system(size: 11)).foregroundStyle(.secondary).monospacedDigit()
                }
                Spacer(minLength: 8)
                trailing()
            }
            .padding(.leading, 16).padding(.trailing, 12).padding(.vertical, 9)
            content()
        }
        // takes the height its row offers, so cards side by side end level
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .surface()
    }
}

extension SectionCard where Trailing == EmptyView {
    init(_ title: String, icon: String, count: Int? = nil, @ViewBuilder content: @escaping () -> Content) {
        self.init(title, icon: icon, count: count, trailing: { EmptyView() }, content: content)
    }
}

/// A label (first view) and its control (second view) on one line, the label taking what the control leaves and
/// wrapping its explanation; only when that leaves the label less than `minLabel` does the control go under it.
struct LabelControlRow: Layout {
    var minLabel: CGFloat = 110
    var spacing: CGFloat = 12

    private func inline(_ w: CGFloat, _ control: CGSize) -> Bool { w - control.width - spacing >= minLabel }

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let control = subviews[1].sizeThatFits(.unspecified)
        let w = proposal.width ?? (subviews[0].sizeThatFits(.unspecified).width + spacing + control.width)
        if inline(w, control) {
            let label = subviews[0].sizeThatFits(.init(width: w - control.width - spacing, height: nil))
            return CGSize(width: w, height: max(label.height, control.height))
        }
        let label = subviews[0].sizeThatFits(.init(width: w, height: nil))
        return CGSize(width: w, height: label.height + 8 + control.height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let control = subviews[1].sizeThatFits(.unspecified)
        let w = bounds.width
        if inline(w, control) {
            let lw = w - control.width - spacing
            subviews[0].place(at: CGPoint(x: bounds.minX, y: bounds.midY), anchor: .leading,
                              proposal: .init(width: lw, height: nil))
            subviews[1].place(at: CGPoint(x: bounds.maxX, y: bounds.midY), anchor: .trailing, proposal: .unspecified)
        } else {
            let lh = subviews[0].sizeThatFits(.init(width: w, height: nil)).height
            subviews[0].place(at: bounds.origin, proposal: .init(width: w, height: lh))
            subviews[1].place(at: CGPoint(x: bounds.minX, y: bounds.minY + lh + 8), proposal: .unspecified)
        }
    }
}

/// Cards in as many equal columns as fit at `minItem` wide — CSS `repeat(auto-fit, minmax(minItem, 1fr))` — with
/// the column count kept to a divisor of the card count, so four cards go 4, 2 or 1 across, never 3 + 1.
/// Cards in a row share the row's height, like a grid row.
struct AutoGrid: Layout {
    var minItem: CGFloat
    var spacing: CGFloat = 12

    private func columns(_ width: CGFloat, _ count: Int) -> Int {
        guard count > 0 else { return 1 }
        var c = max(1, min(count, Int((width + spacing) / (minItem + spacing))))
        while c > 1 && count % c != 0 { c -= 1 }
        return c
    }

    private func rows(_ width: CGFloat, _ subviews: Subviews) -> (cols: Int, item: CGFloat, heights: [CGFloat]) {
        let cols = columns(width, subviews.count)
        let item = ((width - spacing * CGFloat(cols - 1)) / CGFloat(cols)).rounded(.down)
        let heights = stride(from: 0, to: subviews.count, by: cols).map { start in
            subviews[start..<min(start + cols, subviews.count)]
                .map { $0.sizeThatFits(.init(width: item, height: nil)).height }.max() ?? 0
        }
        return (cols, item, heights)
    }

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let w = proposal.width ?? minItem
        let r = rows(w, subviews)
        return CGSize(width: w, height: r.heights.reduce(0, +) + spacing * CGFloat(max(0, r.heights.count - 1)))
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let r = rows(bounds.width, subviews)
        var y = bounds.minY
        for (row, h) in r.heights.enumerated() {
            for col in 0..<r.cols {
                let i = row * r.cols + col
                guard i < subviews.count else { break }
                subviews[i].place(at: CGPoint(x: bounds.minX + CGFloat(col) * (r.item + spacing), y: y),
                                  proposal: .init(width: r.item, height: h))
            }
            y += h + spacing
        }
    }
}

/// Two views side by side at `ratio` : 1 when the offered width allows `minWidth`, else one above the other —
/// flex with wrapping. It only reads the width it is offered, so it never pushes the window wider.
struct SplitRow: Layout {
    var ratio: CGFloat = 1
    var spacing: CGFloat = 14
    var minWidth: CGFloat = 760
    /// Side by side, give both the taller one's height, so two cards end level.
    var matchHeights = false

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let w = proposal.width ?? minWidth
        if sideBySide(w, subviews) {
            let (a, b) = widths(w)
            let h = max(subviews[0].sizeThatFits(.init(width: a, height: nil)).height,
                        subviews[1].sizeThatFits(.init(width: b, height: nil)).height)
            return CGSize(width: w, height: h)
        }
        let heights = subviews.map { $0.sizeThatFits(.init(width: w, height: nil)).height }
        return CGSize(width: w, height: heights.reduce(0, +) + spacing * CGFloat(max(0, heights.count - 1)))
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let w = bounds.width
        if sideBySide(w, subviews) {
            let (a, b) = widths(w)
            // each column keeps its own height (top aligned), or both take the row's
            let h: CGFloat? = matchHeights ? bounds.height : nil
            subviews[0].place(at: bounds.origin, proposal: .init(width: a, height: h))
            subviews[1].place(at: CGPoint(x: bounds.minX + a + spacing, y: bounds.minY), proposal: .init(width: b, height: h))
        } else {
            var y = bounds.minY
            for s in subviews {
                let h = s.sizeThatFits(.init(width: w, height: nil)).height
                s.place(at: CGPoint(x: bounds.minX, y: y), proposal: .init(width: w, height: h))
                y += h + spacing
            }
        }
    }

    private func sideBySide(_ w: CGFloat, _ subviews: Subviews) -> Bool { subviews.count == 2 && w >= minWidth }

    private func widths(_ w: CGFloat) -> (CGFloat, CGFloat) {
        let a = ((w - spacing) * ratio / (ratio + 1)).rounded(.down)
        return (a, w - spacing - a)
    }
}

/// A settings line: label (and an optional explanation under it) on the left, the control on the right; when the
/// card is too narrow for both, the control goes under the label.
struct SettingRow<Trailing: View>: View {
    let label: String
    var caption: String?
    let trailing: () -> Trailing

    init(_ label: String, caption: String? = nil, @ViewBuilder trailing: @escaping () -> Trailing) {
        self.label = label
        self.caption = caption
        self.trailing = trailing
    }

    var body: some View {
        LabelControlRow {
            labels
            trailing().opacity(isEnabled ? 1 : 0.5)       // a control that cannot be used now looks it
        }
        .cardLine()
    }

    @Environment(\.isEnabled) private var isEnabled

    private var labels: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(label).font(.system(size: 12, weight: .medium))
                .foregroundStyle(isEnabled ? Color.primary : Color.secondary)
            if let caption {
                Text(caption).font(.system(size: 11)).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

/// Capsule buttons. `.normal` for most actions, `.primary` for the one main action of a page, `.tinted` for the
/// main action of a row.
struct PillButtonStyle: ButtonStyle {
    enum Kind { case normal, primary, tinted }
    var kind: Kind = .normal
    var height: CGFloat = Metrics.control

    func makeBody(configuration: Configuration) -> some View {
        PillBody(label: configuration.label, pressed: configuration.isPressed, kind: kind, height: height)
    }

    private struct PillBody<Label: View>: View {
        let label: Label
        let pressed: Bool
        let kind: Kind
        let height: CGFloat
        @Environment(\.isEnabled) private var isEnabled

        var body: some View {
            label
                .font(.system(size: Metrics.body, weight: .medium))     // one weight for every button; colour marks the main one
                .lineLimit(1)
                .fixedSize(horizontal: true, vertical: false)      // a button's title is never cut short
                .padding(.horizontal, 12)
                .frame(height: height)
                .foregroundStyle(foreground)
                .background(background, in: Capsule())
                .overlay(Capsule().strokeBorder(border))
                .opacity(isEnabled ? 1 : 0.45)
                .contentShape(Capsule())
        }

        private var foreground: Color {
            switch kind {
            case .normal: return .primary
            case .primary: return .white
            case .tinted: return .accentColor
            }
        }

        private var background: Color {
            switch kind {
            case .normal: return Color.primary.opacity(pressed ? 0.14 : 0.08)
            case .primary: return Color.accentColor.opacity(pressed ? 0.8 : 1)
            case .tinted: return Color.accentColor.opacity(pressed ? 0.28 : 0.18)
            }
        }

        private var border: Color {
            switch kind {
            case .normal: return fieldStroke
            case .primary: return Color.white.opacity(0.2)
            case .tinted: return Color.accentColor.opacity(0.4)
            }
        }
    }
}

extension ButtonStyle where Self == PillButtonStyle {
    static var pill: PillButtonStyle { PillButtonStyle() }
    static var pillPrimary: PillButtonStyle { PillButtonStyle(kind: .primary) }
    static var pillTinted: PillButtonStyle { PillButtonStyle(kind: .tinted) }
}

/// An icon-only round button (⋯, mute, refresh).
struct RoundIconButton: View {
    let systemName: String
    let help: String
    var size: CGFloat = 22
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            Image(systemName: systemName).font(.system(size: 12, weight: .medium))
                .frame(width: size, height: size).contentShape(Circle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(.secondary)
        .help(help)
        .accessibilityLabel(help)
    }
}

/// The value field of a choice: shows the current value, opens a menu of the choices.
struct ValueMenu<Content: View>: View {
    let value: String
    let content: () -> Content

    init(_ value: String, @ViewBuilder content: @escaping () -> Content) {
        self.value = value
        self.content = content
    }

    var body: some View {
        Menu(content: content) {
            HStack(spacing: 8) {
                Text(value).font(.system(size: Metrics.body, weight: .medium)).monospacedDigit()
                Image(systemName: "chevron.up.chevron.down").font(.system(size: 9, weight: .semibold))
                    .foregroundStyle(.secondary)
            }
        }
        .menuStyle(.button)
        .buttonStyle(FieldButtonStyle())
        .menuIndicator(.hidden)
        .fixedSize()
    }
}

/// The look of a field-like button (ValueMenu).
struct FieldButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: Metrics.body, weight: .medium))
            .padding(.leading, 12).padding(.trailing, 10)
            .frame(height: Metrics.control)
            .background(Color.primary.opacity(configuration.isPressed ? 0.14 : 0.08), in: Capsule())
            .overlay(Capsule().strokeBorder(Color.primary.opacity(0.10)))
            .contentShape(Capsule())
    }
}

/// Two or three mutually exclusive choices side by side.
struct SegmentedPill<Value: Hashable>: View {
    @Binding var selection: Value
    let options: [(Value, String)]
    @Environment(\.isEnabled) private var isEnabled

    var body: some View {
        HStack(spacing: 0) {
            ForEach(options.indices, id: \.self) { i in
                let (value, label) = options[i]
                let on = selection == value
                Button { selection = value } label: {
                    Text(label)
                        .font(.system(size: Metrics.body, weight: on ? .semibold : .medium))
                        .padding(.horizontal, 12).frame(height: Metrics.control - 4)
                        .foregroundStyle(on ? (isEnabled ? Color.white : Color.primary) : Color.secondary)
                        .background(on ? (isEnabled ? Color.accentColor : Color.primary.opacity(0.14)) : Color.clear,
                                    in: Capsule())
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
        .padding(2)
        .background(fieldFill, in: Capsule())
        .overlay(Capsule().strokeBorder(Color.primary.opacity(0.10)))
        .fixedSize()
    }
}

/// Two icon buttons side by side, one of them chosen (list / grid).
struct IconSegmented: View {
    @Binding var selection: Bool
    let off: (String, String)          // (SF Symbol, help) for false
    let on: (String, String)           // for true

    var body: some View {
        HStack(spacing: 0) {
            item(off, value: false)
            item(on, value: true)
        }
        .padding(2)
        .background(fieldFill, in: Capsule())
        .overlay(Capsule().strokeBorder(Color.primary.opacity(0.10)))
    }

    private func item(_ spec: (String, String), value: Bool) -> some View {
        let chosen = selection == value
        return Button { selection = value } label: {
            Image(systemName: spec.0).font(.system(size: Metrics.caption, weight: .medium))
                .frame(width: 30, height: Metrics.control - 4)
                .foregroundStyle(chosen ? Color.primary : Color.secondary)
                .background(chosen ? Color.primary.opacity(0.14) : Color.clear, in: Capsule())
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(spec.1)
        .accessibilityLabel(spec.1)
        .accessibilityAddTraits(chosen ? .isSelected : [])
    }
}

/// A round-cornered letter badge standing in for an app's icon. The colour follows the package name, so an
/// app keeps its colour between launches.
struct LetterAvatar: View {
    let title: String
    let seed: String
    var size: CGFloat = 28

    private static let palettes: [(Color, Color)] = [
        (Color(red: 0.55, green: 0.42, blue: 1.00), Color(red: 0.23, green: 0.16, blue: 0.62)),
        (Color(red: 0.13, green: 0.72, blue: 0.69), Color(red: 0.04, green: 0.33, blue: 0.38)),
        (Color(red: 0.30, green: 0.55, blue: 1.00), Color(red: 0.10, green: 0.25, blue: 0.62)),
        (Color(red: 1.00, green: 0.58, blue: 0.27), Color(red: 0.70, green: 0.27, blue: 0.08)),
        (Color(red: 0.95, green: 0.38, blue: 0.56), Color(red: 0.58, green: 0.13, blue: 0.32)),
        (Color(red: 0.36, green: 0.80, blue: 0.43), Color(red: 0.10, green: 0.45, blue: 0.22)),
    ]

    var body: some View {
        let sum = seed.unicodeScalars.reduce(0) { ($0 &* 31 &+ Int($1.value)) & 0x7fffffff }
        let (top, bottom) = Self.palettes[sum % Self.palettes.count]
        Text(String(title.trimmingCharacters(in: .whitespaces).prefix(1)).uppercased())
            .font(.system(size: size * 0.43, weight: .bold))
            .foregroundStyle(.white)
            .frame(width: size, height: size)
            .background(LinearGradient(colors: [top, bottom], startPoint: .topLeading, endPoint: .bottomTrailing),
                        in: RoundedRectangle(cornerRadius: size * 0.28, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: size * 0.28, style: .continuous)
                .strokeBorder(Color.white.opacity(0.14)))
            .accessibilityHidden(true)
    }
}

/// A small text tag in place of an icon (a script's "LUA").
struct TagAvatar: View {
    let text: String
    var size: CGFloat = 28
    var body: some View {
        Text(text)
            .font(.system(size: size > 36 ? 11 : 10, weight: .semibold, design: .monospaced))
            .tracking(0.4)
            .foregroundStyle(Color.accentColor)
            .frame(width: size, height: size)
            .background(Color.accentColor.opacity(0.14), in: RoundedRectangle(cornerRadius: size * 0.28, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: size * 0.28, style: .continuous).strokeBorder(Color.accentColor.opacity(0.28)))
            .accessibilityHidden(true)
    }
}

/// A coloured status capsule ("● Running", "Stop it to change these").
struct StatusPill: View {
    let text: String
    let color: Color
    var systemImage: String?

    var body: some View {
        HStack(spacing: 6) {
            if let systemImage {
                Image(systemName: systemImage).font(.system(size: 10, weight: .bold))
            } else {
                Circle().fill(color).frame(width: 6, height: 6)
            }
            Text(text)
        }
        .font(.system(size: 11, weight: .semibold))
        .foregroundStyle(color)
        .padding(.horizontal, 8).frame(height: 19)
        .background(color.opacity(0.14), in: Capsule())
        .overlay(Capsule().strokeBorder(color.opacity(0.3)))
    }
}

/// A slim progress bar.
struct MeterBar: View {
    let fraction: Double
    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(Color.primary.opacity(0.08))
                Capsule().fill(fraction > 0.9 ? Color.red : Color.accentColor)
                    .frame(width: max(4, geo.size.width * min(max(fraction, 0), 1)))
            }
        }
        .frame(height: 4)
    }
}

/// One live figure: label and side note, the number, a meter.
struct StatCard: View {
    let icon: String
    let label: String
    var note: String?
    let value: String
    var unit: String?
    var fraction: Double?
    var help: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            HStack(spacing: 6) {
                Image(systemName: icon).font(.system(size: 11, weight: .semibold))
                Text(label).font(.system(size: 11, weight: .semibold))
                Spacer(minLength: 4)
                if let note { Text(note).font(.system(size: 11)).monospacedDigit() }
            }
            .foregroundStyle(.secondary)
            HStack(alignment: .firstTextBaseline, spacing: 4) {
                Text(value).font(.system(size: 20, weight: .semibold)).monospacedDigit()
                if let unit { Text(unit).font(.system(size: 13, weight: .medium)).foregroundStyle(.secondary) }
            }
            .lineLimit(1).minimumScaleFactor(0.7)
            if let fraction { MeterBar(fraction: fraction) } else { Color.clear.frame(height: 4) }
        }
        .padding(.horizontal, 15).padding(.vertical, 13)
        .frame(maxWidth: .infinity, alignment: .leading)
        .surface(18)
        .help(help ?? "")
    }
}

/// A capsule holding a few header controls (volume, record | stop).
struct ControlCapsule<Content: View>: View {
    let content: () -> Content
    init(@ViewBuilder content: @escaping () -> Content) { self.content = content }
    var body: some View {
        HStack(spacing: 4, content: content)
            .padding(.horizontal, 2)
            .frame(height: Metrics.control)                  // as tall as the buttons beside it
            .glassPanel(cornerRadius: Metrics.control / 2)
            .overlay(Capsule().strokeBorder(Color.primary.opacity(0.10)))
    }
}

/// A button inside a ControlCapsule.
struct CapsuleItemStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: Metrics.body, weight: .medium))
            .lineLimit(1)
            .fixedSize(horizontal: true, vertical: false)
            .padding(.horizontal, 10).frame(height: Metrics.control - 4)
            .background(Color.primary.opacity(configuration.isPressed ? 0.12 : 0), in: Capsule())
            .contentShape(Capsule())
    }
}
