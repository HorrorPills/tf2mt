// tf2mt launcher — Team Fortress 2 tuned for Apple Silicon (native SwiftUI, built by tools/launcher/build.sh).
// Self-contained: the tf2mt scripts/configs/patch tool are bundled in Resources/tf2mt (a dev build without them
// falls back to ~/Code/tf2mt). No Valve content is bundled: TF2 art and fonts are loaded at runtime from the
// user's own installation. All game work is done by the bundled shell scripts; the app passes options as
// environment variables (TF2_HOME, DXVK_CONFIG_FILE, MTL_HUD_ENABLED, TF2_FRIENDS_ONLINE) and launch arguments.
import SwiftUI
import AppKit
import CoreText
import UniformTypeIdentifiers

// MARK: - paths & shell

func exists(_ path: String) -> Bool { FileManager.default.fileExists(atPath: path) }

enum Paths {
    static let home = FileManager.default.homeDirectoryForCurrentUser.path
    static var repo: String {
        let bundled = (Bundle.main.resourcePath ?? "") + "/tf2mt"
        return exists(bundled + "/scripts/play.sh") ? bundled : home + "/Code/tf2mt"
    }
    static var game: String { UserDefaults.standard.string(forKey: "gameHome") ?? home + "/Games/tf2" }
    static var script: (String) -> String { { repo + "/scripts/" + $0 } }
    static var mousePatch: String { repo + "/tools/wine-patches/winemac_warp_nodiscard.sh" }
    static var releaseConf: String { repo + "/config/release.conf" }
    static var logs: String { game + "/logs" }
    static var wine: String { game + "/wine/bin/wine" }
    static var prefix: String { game + "/prefix" }
    static var steamDir: String { prefix + "/drive_c/Program Files (x86)/Steam" }
    static var steamExe: String { steamDir + "/steam.exe" }
    static var tf2Dir: String { steamDir + "/steamapps/common/Team Fortress 2" }
    static var tf2Manifest: String { steamDir + "/steamapps/appmanifest_440.acf" }
    static var art: String { steamDir + "/appcache/librarycache/440" }
    static var dxvkTearFree: String { game + "/config/dxvk.conf" }
    static var dxvkImmediate: String { game + "/config/dxvk-immediate.conf" }
    static var connectionLog: String { steamDir + "/logs/connection_log.txt" }
    static let rosetta = "/Library/Apple/usr/libexec/oah/libRosettaRuntime"
}

/// Environment for every script: the caller's environment plus the configured game folder.
func scriptEnv(_ extra: [String: String] = [:], removing: [String] = []) -> [String: String] {
    var env = ProcessInfo.processInfo.environment
    env["TF2_HOME"] = Paths.game
    removing.forEach { env.removeValue(forKey: $0) }
    extra.forEach { env[$0.key] = $0.value }
    return env
}

@discardableResult
func shell(_ args: [String], env: [String: String]? = nil) -> (status: Int32, output: String) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: args[0])
    p.arguments = Array(args.dropFirst())
    p.environment = env ?? scriptEnv()
    let pipe = Pipe()
    p.standardOutput = pipe
    p.standardError = pipe
    do { try p.run() } catch { return (-1, "\(error)") }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    return (p.terminationStatus, String(decoding: data, as: UTF8.self))
}

// MARK: - Valve art & fonts from the user's install (never bundled)

func gameArt(_ name: String) -> NSImage? {
    let path = Paths.art + "/" + name
    return exists(path) ? NSImage(contentsOfFile: path) : nil
}

func registerGameFonts() {
    for f in ["tf2build.ttf", "tf2secondary.ttf"] {
        let url = URL(fileURLWithPath: Paths.tf2Dir + "/tf/resource/" + f)
        if exists(url.path) { CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil) }
    }
}

// MARK: - frame pacing

/// How frames are paced. Stored as a string in UserDefaults ("vsync", "uncapped", "cap:144").
struct Pacing: Hashable, Identifiable {
    let raw: String
    var id: String { raw }
    static let vsync = Pacing(raw: "vsync")
    static let uncapped = Pacing(raw: "uncapped")
    static func cap(_ fps: Int) -> Pacing { Pacing(raw: "cap:\(fps)") }
    var capFPS: Int? { raw.hasPrefix("cap:") ? Int(raw.dropFirst(4)) : nil }

    func label(displayHz: Int) -> String {
        switch raw {
        case "vsync": return "Vsync — match display (\(displayHz) Hz)"
        case "uncapped": return "Uncapped (fastest, may tear)"
        default: return "Cap at \(capFPS ?? 0) fps (no vsync)"
        }
    }
    var dxvkConfig: String { raw == "vsync" ? Paths.dxvkTearFree : Paths.dxvkImmediate }
    var launchArgs: [String] {
        switch raw {
        case "vsync": return []
        case "uncapped": return ["+fps_max", "0"]
        default: return ["+fps_max", String(capFPS ?? 0)]
        }
    }
    static func options(displayHz: Int) -> [Pacing] {
        let caps = Set([60, 120, 144, 165, 240, displayHz]).filter { $0 <= max(displayHz, 60) * 2 }.sorted()
        return [.vsync, .uncapped] + caps.map { .cap($0) }
    }
}

var displayRefreshHz: Int { max(NSScreen.main?.maximumFramesPerSecond ?? 60, 30) }
var displayName: String { NSScreen.main?.localizedName ?? "Display" }

// MARK: - model

enum SteamState { case stopped, starting, online }
enum MouseFix { case applied, original, unknown }

struct SetupCheck: Identifiable {
    let id: String
    let title: String
    let ok: Bool
    let fix: String
}

@MainActor
final class Launcher: ObservableObject {
    @Published var steam: SteamState = .stopped
    @Published var tf2Running = false
    @Published var launching = false
    @Published var mouseFix: MouseFix = .unknown
    @Published var comfigPreset = "none"       // mastercomfig preset from setup_hook.cfg ("none" = not set)
    @Published var comfigInstalled = false
    @Published var checks: [SetupCheck] = []
    @Published var message: String?
    @Published var setupRunning = false
    @Published var setupLog: [String] = []
    @Published var artVersion = 0          // bumps when the game folder changes, to reload art

    private var timer: Timer?

    /// Everything required to play (the mouse fix is recommended, not required).
    var readyToPlay: Bool { !checks.isEmpty && checks.filter { $0.id != "mouse" }.allSatisfy(\.ok) }
    var setupComplete: Bool { !checks.isEmpty && checks.allSatisfy(\.ok) }
    var everLoggedIn: Bool { ((try? String(contentsOfFile: Paths.connectionLog, encoding: .utf8)) ?? "").contains("Logged On") }

    init() {
        registerGameFonts()
        refresh()
        refreshMouseFix(); refreshComfig()
        refreshChecks()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    func refresh() {
        Task.detached {
            // TF2 / Steam: only processes whose argv[0] is the Windows exe (same rules as scripts/_env.sh)
            func running(_ pattern: String, _ argv0: (Substring) -> Bool) -> Bool {
                shell(["/usr/bin/pgrep", "-fl", pattern]).output.split(separator: "\n").contains { line in
                    let parts = line.split(separator: " ", maxSplits: 2)
                    return parts.count >= 2 && argv0(parts[1])
                }
            }
            let tf2 = running("tf_win64\\.exe") { $0.hasPrefix("C:") }
            let steamUp = running("steam\\.exe") { $0 == "steam.exe" || $0.hasSuffix("\\steam.exe") }
            let loggedOn: Bool = {
                guard steamUp, let log = try? String(contentsOfFile: Paths.connectionLog, encoding: .utf8) else { return false }
                let states = log.split(separator: "\n").suffix(400)
                    .filter { $0.contains("[Logged On") || $0.contains("[Logged Off") || $0.contains("[Logging On") }
                return states.last?.contains("[Logged On") ?? false
            }()
            await MainActor.run {
                self.tf2Running = tf2
                self.steam = steamUp ? (loggedOn ? .online : .starting) : .stopped
                if tf2 { self.launching = false }
            }
        }
    }

    func refreshComfig() {
        Task.detached {
            let out = shell(["/bin/bash", Paths.script("mastercomfig.sh"), "status"]).output
            let preset = out.components(separatedBy: "preset=").last?.trimmingCharacters(in: .whitespacesAndNewlines) ?? "none"
            await MainActor.run { self.comfigInstalled = out.contains("installed=yes"); self.comfigPreset = preset.isEmpty ? "none" : preset }
        }
    }

    func setComfig(_ preset: String) {
        guard preset != comfigPreset, preset != "none" else { return }
        comfigPreset = preset
        message = comfigInstalled ? "Switching mastercomfig to \(preset.capitalized)…" : "Downloading mastercomfig from GitHub…"
        Task.detached {
            let r = shell(["/bin/bash", Paths.script("mastercomfig.sh"), "set", preset])
            await MainActor.run {
                let out = r.output.trimmingCharacters(in: .whitespacesAndNewlines)
                self.message = r.status == 0
                    ? "mastercomfig \(preset.capitalized) selected" + (self.tf2Running ? " — restart TF2 to apply." : " — applies on next launch.")
                    : "mastercomfig: " + out
                self.refreshComfig()
            }
        }
    }

    func refreshMouseFix() {
        Task.detached {
            guard exists(Paths.mousePatch), exists(Paths.wine) else {
                await MainActor.run { self.mouseFix = .unknown; self.refreshChecks() }; return
            }
            let out = shell(["/bin/bash", Paths.mousePatch, "status"]).output
            let state: MouseFix = out.hasPrefix("patched") ? .applied : out.hasPrefix("original") ? .original : .unknown
            await MainActor.run { self.mouseFix = state; self.refreshChecks() }
        }
    }

    func refreshChecks() {
        let manifest = (try? String(contentsOfFile: Paths.tf2Manifest, encoding: .utf8)) ?? ""
        let tf2Installed = manifest.range(of: #""StateFlags"\s+"4""#, options: .regularExpression) != nil
        checks = [
            SetupCheck(id: "rosetta", title: "Rosetta 2", ok: exists(Paths.rosetta),
                       fix: "Click “Run setup” (or: softwareupdate --install-rosetta --agree-to-license)."),
            SetupCheck(id: "wine", title: "Wine runtime", ok: exists(Paths.wine),
                       fix: "Click “Run setup” — downloads the tf2mt runtime (Wine 10 + DXVK + MoltenVK, ~230 MB)."),
            SetupCheck(id: "prefix", title: "Wine prefix with DXVK", ok: exists(Paths.prefix + "/system.reg"),
                       fix: "Created by “Run setup”."),
            SetupCheck(id: "steam", title: "Steam for Windows", ok: exists(Paths.steamExe),
                       fix: "Installed by “Run setup” (official Valve installer)."),
            SetupCheck(id: "login", title: "Logged in to Steam", ok: everLoggedIn,
                       fix: "Click “Log in to Steam” and sign in once (Steam Guard as usual)."),
            SetupCheck(id: "tf2", title: "Team Fortress 2 installed", ok: tf2Installed,
                       fix: "Click “Install TF2” (free, ≈31 GB download in Steam)."),
            SetupCheck(id: "config", title: "tf2mt configs", ok: exists(Paths.dxvkTearFree) && exists(Paths.dxvkImmediate),
                       fix: "Installed by “Run setup”."),
            SetupCheck(id: "mouse", title: "Smooth mouse fix", ok: mouseFix == .applied,
                       fix: mouseFix == .unknown ? "Needs the tf2mt runtime (patch matches that exact Wine build)."
                                                 : "Turn on “Smooth mouse fix” in Settings or click “Run setup”."),
        ]
    }

    // MARK: setup

    func runSetup(runtimeFile: String? = nil) {
        guard !setupRunning else { return }
        setupRunning = true
        setupLog = []
        var args = ["/bin/bash", Paths.script("setup.sh")]
        if let runtimeFile { args += ["--runtime", runtimeFile] }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: args[0])
        p.arguments = Array(args.dropFirst())
        p.environment = scriptEnv()
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        pipe.fileHandleForReading.readabilityHandler = { h in
            let text = String(decoding: h.availableData, as: UTF8.self)
            let lines = text.split(separator: "\n").map(String.init)
            if !lines.isEmpty { Task { @MainActor in self.setupLog += lines } }
        }
        p.terminationHandler = { proc in
            pipe.fileHandleForReading.readabilityHandler = nil
            Task { @MainActor in
                self.setupRunning = false
                self.message = proc.terminationStatus == 0 ? "Setup finished." : "Setup stopped — see the log above."
                registerGameFonts(); self.artVersion += 1
                self.refreshMouseFix(); self.refreshComfig(); self.refresh()
            }
        }
        do { try p.run() } catch { setupRunning = false; setupLog = ["FAIL \(error)"] }
    }

    /// Runtime URL configured for this release (config/release.conf), if any.
    var runtimeURLConfigured: Bool {
        let conf = (try? String(contentsOfFile: Paths.releaseConf, encoding: .utf8)) ?? ""
        return conf.range(of: #"RUNTIME_URL="https?://"#, options: .regularExpression) != nil
    }

    func runSetupInteractive() {
        if exists(Paths.wine) || runtimeURLConfigured { runSetup(); return }
        let panel = NSOpenPanel()
        panel.message = "Choose the tf2mt runtime (tf2mt-runtime-*.tar.xz) downloaded from the tf2mt release page"
        panel.allowedContentTypes = [UTType(filenameExtension: "xz") ?? .data]
        if panel.runModal() == .OK, let url = panel.url { runSetup(runtimeFile: url.path) }
    }

    func loginSteam() {
        message = "Opening Steam — sign in, then come back here."
        Task.detached { shell(["/bin/bash", Paths.script("steam.sh"), "--login"], env: scriptEnv(removing: ["MTL_HUD_ENABLED"])) }
    }

    func installTF2() {
        Task.detached {
            let r = shell(["/bin/bash", Paths.script("install-tf2.sh")])
            await MainActor.run { self.message = r.output.trimmingCharacters(in: .whitespacesAndNewlines) }
        }
    }

    func chooseGameFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.message = "Choose where tf2mt keeps Wine, Steam and TF2 (needs ~35 GB)"
        if panel.runModal() == .OK, let url = panel.url {
            UserDefaults.standard.set(url.path, forKey: "gameHome")
            registerGameFonts(); artVersion += 1
            refreshMouseFix(); refreshComfig(); refresh()
        }
    }

    // MARK: play & control

    func setMouseFix(_ on: Bool) {
        Task.detached {
            let r = shell(["/bin/bash", Paths.mousePatch, on ? "apply" : "revert"])
            await MainActor.run {
                self.message = r.status == 0
                    ? (on ? "Mouse fix applied — takes effect on next launch." : "Mouse fix removed — takes effect on next launch.")
                    : "Mouse fix: " + r.output.trimmingCharacters(in: .whitespacesAndNewlines)
                self.refreshMouseFix(); self.refreshComfig()
            }
        }
    }

    func play(pacing: Pacing, hud: Bool, friendsOffline: Bool, metalRenderer: Bool, frameLog: Bool, extraArgs: String) {
        guard !launching, !tf2Running else { return }
        refreshChecks()
        guard readyToPlay else { message = "Setup incomplete — see the Setup tab."; return }
        launching = true
        message = steam == .online ? "Starting Team Fortress 2…" : "Starting Steam — waiting for login…"
        var extra = ["DXVK_CONFIG_FILE": pacing.dxvkConfig, "TF2_FRIENDS_ONLINE": friendsOffline ? "0" : "1"]
        if hud { extra["MTL_HUD_ENABLED"] = "1" }
        extra["TF2_RENDERER"] = metalRenderer ? "tf2mt" : "dxvk"
        extra["TF2_FRAMELOG"] = frameLog ? "1" : "0"
        extra["TF2MT_VSYNC"] = pacing.raw == "vsync" ? "1" : "0"   // Metal renderer follows Frame pacing too
        let env = scriptEnv(extra, removing: hud ? [] : ["MTL_HUD_ENABLED"])
        let args = ["/bin/bash", Paths.script("play.sh")] + pacing.launchArgs + extraArgs.split(separator: " ").map(String.init)
        let firstPreset = comfigPreset == "none"   // new install: mastercomfig Low (recommended) before the first launch
        Task.detached {
            if firstPreset { shell(["/bin/bash", Paths.script("mastercomfig.sh"), "set", "low"]) }
            try? FileManager.default.createDirectory(atPath: Paths.logs, withIntermediateDirectories: true)
            let r = shell(args, env: env)
            let logURL = URL(fileURLWithPath: Paths.logs + "/launcher.log")
            if let h = try? FileHandle(forWritingTo: logURL) {
                h.seekToEndOfFile(); h.write(Data(("=== tf2mt launcher \(Date())\n" + r.output).utf8)); try? h.close()
            } else {
                try? r.output.write(to: logURL, atomically: true, encoding: .utf8)
            }
            await MainActor.run {
                self.launching = false
                self.message = r.status == 0 ? "Have fun!" : "Launch failed — open Logs for details."
                self.refresh(); if firstPreset { self.refreshComfig() }
            }
        }
    }

    func quitTF2() {
        Task.detached {
            for line in shell(["/usr/bin/pgrep", "-fl", "tf_win64\\.exe"]).output.split(separator: "\n") {
                let parts = line.split(separator: " ", maxSplits: 2)
                if parts.count >= 2, parts[1].hasPrefix("C:"), let pid = Int32(parts[0]) { kill(pid, SIGTERM) }
            }
            await MainActor.run { self.message = "TF2 closed."; self.refresh() }
        }
    }

    func startSteam() {
        if !everLoggedIn { loginSteam(); return }
        message = "Starting Steam…"
        Task.detached { shell(["/bin/bash", Paths.script("steam.sh")], env: scriptEnv(removing: ["MTL_HUD_ENABLED"])) }
    }

    func openLogs() {
        try? FileManager.default.createDirectory(atPath: Paths.logs, withIntermediateDirectories: true)
        NSWorkspace.shared.open(URL(fileURLWithPath: Paths.logs))
    }
}

// MARK: - style

extension Color {
    static let tfOrange = Color(red: 0.81, green: 0.42, blue: 0.20)      // #CF6A32
    static let tfOrangeLight = Color(red: 0.93, green: 0.55, blue: 0.27)
    static let tfCream = Color(red: 0.92, green: 0.89, blue: 0.80)
    static let tfDark = Color(red: 0.11, green: 0.10, blue: 0.09)
    static let tfPanel = Color(red: 0.17, green: 0.15, blue: 0.14)
    static let tfRed = Color(red: 0.72, green: 0.22, blue: 0.20)
    static let tfGreen = Color(red: 0.45, green: 0.70, blue: 0.35)
}

extension Font {
    // TF2's fonts when the game is installed (registered at runtime); SwiftUI falls back to the system font otherwise
    static func build(_ size: CGFloat) -> Font { .custom("TF2Build", size: size) }
    static func secondary(_ size: CGFloat) -> Font { .custom("TF2Secondary", size: size) }
}

struct Card<Content: View>: View {
    let title: String
    @ViewBuilder var content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if !title.isEmpty {
                Text(title.uppercased()).font(.build(15)).foregroundStyle(Color.tfOrangeLight).tracking(1.5)
            }
            content
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.tfPanel.opacity(0.92), in: RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(Color.white.opacity(0.06)))
    }
}

struct RowText: View {
    let title: String
    let detail: String
    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.secondary(15)).foregroundStyle(Color.tfCream)
            Text(detail).font(.system(size: 11)).foregroundStyle(Color.tfCream.opacity(0.55))
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct OptionRow: View {
    let title: String
    let detail: String
    @Binding var isOn: Bool
    var body: some View {
        Toggle(isOn: $isOn) { RowText(title: title, detail: detail) }
            .toggleStyle(.switch).tint(.tfOrange)
    }
}

struct StatusRow: View {
    let label: String
    let value: String
    let color: Color
    var body: some View {
        HStack {
            Circle().fill(color).frame(width: 9, height: 9).shadow(color: color.opacity(0.8), radius: 4)
            Text(label).font(.secondary(14)).foregroundStyle(Color.tfCream.opacity(0.75))
            Spacer()
            Text(value).font(.secondary(14)).foregroundStyle(Color.tfCream)
        }
    }
}

struct PlayButtonStyle: ButtonStyle {
    let enabled: Bool
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.build(34))
            .foregroundStyle(.white)
            .frame(maxWidth: .infinity, minHeight: 74)
            .background(
                LinearGradient(colors: enabled ? [.tfOrangeLight, .tfOrange] : [.gray.opacity(0.5), .gray.opacity(0.35)],
                               startPoint: .top, endPoint: .bottom),
                in: RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Color.white.opacity(0.25), lineWidth: 1))
            .shadow(color: enabled ? Color.tfOrange.opacity(0.55) : .clear, radius: configuration.isPressed ? 4 : 14, y: 4)
            .scaleEffect(configuration.isPressed ? 0.98 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

struct SmallButtonStyle: ButtonStyle {
    var prominent = false
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.secondary(13))
            .foregroundStyle(prominent ? Color.white : Color.tfCream)
            .padding(.vertical, 8).frame(maxWidth: .infinity)
            .background((prominent ? Color.tfOrange : Color.white.opacity(0.08)).opacity(configuration.isPressed ? 0.7 : 1),
                        in: RoundedRectangle(cornerRadius: 8))
    }
}

struct TabButton: View {
    let title: String
    let selected: Bool
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            Text(title.uppercased()).font(.build(14)).tracking(1.2)
                .foregroundStyle(selected ? Color.white : Color.tfCream.opacity(0.55))
                .padding(.horizontal, 14).padding(.vertical, 7)
                .background(selected ? Color.tfOrange : Color.clear, in: RoundedRectangle(cornerRadius: 7))
        }
        .buttonStyle(.plain)
    }
}

// MARK: - views

enum Tab: String, CaseIterable { case settings = "Settings", setup = "Setup", debug = "Debug" }

struct ContentView: View {
    @StateObject private var launcher = Launcher()
    @AppStorage("pacing") private var pacingRaw = Pacing.vsync.raw
    @AppStorage("metalHUD") private var metalHUD = false
    @AppStorage("friendsOffline") private var friendsOffline = true
    @AppStorage("metalRenderer") private var metalRenderer = true
    @AppStorage("frameLog") private var frameLog = false
    @AppStorage("extraArgs") private var extraArgs = ""
    @AppStorage("tab") private var tabRaw = Tab.settings.rawValue   // (@State needs the Xcode macro plugin)
    private var tab: Tab { Tab(rawValue: tabRaw) ?? .settings }

    private var pacing: Binding<Pacing> { Binding(get: { Pacing(raw: pacingRaw) }, set: { pacingRaw = $0.raw }) }

    var body: some View {
        ZStack(alignment: .top) {
            Color.tfDark.ignoresSafeArea()
            VStack(spacing: 0) {
                hero
                HStack(alignment: .top, spacing: 18) {
                    VStack(alignment: .leading, spacing: 10) {
                        HStack(spacing: 6) {
                            ForEach(Tab.allCases, id: \.self) { t in
                                TabButton(title: t.rawValue, selected: tab == t) { tabRaw = t.rawValue }
                            }
                            Spacer()
                            if !launcher.setupComplete && tab != .setup {
                                Button { tabRaw = Tab.setup.rawValue } label: {
                                    Label("Setup incomplete", systemImage: "exclamationmark.triangle.fill")
                                        .font(.secondary(13)).foregroundStyle(Color.tfOrangeLight)
                                }.buttonStyle(.plain)
                            }
                        }
                        ScrollView {
                            switch tab {
                            case .settings: settings
                            case .setup: setup
                            case .debug: debug
                            }
                        }
                        .scrollIndicators(.never)
                        .frame(height: 340)
                    }
                    VStack(spacing: 16) {
                        status
                        playButton
                        HStack(spacing: 10) {
                            Button("Quit TF2") { launcher.quitTF2() }.disabled(!launcher.tf2Running)
                            Button("Start Steam") { launcher.startSteam() }.disabled(launcher.steam != .stopped || !exists(Paths.steamExe))
                            Button("Logs") { launcher.openLogs() }
                        }
                        .buttonStyle(SmallButtonStyle())
                        if let m = launcher.message {
                            Text(m).font(.secondary(13)).foregroundStyle(Color.tfCream.opacity(0.7))
                                .multilineTextAlignment(.center)
                                .frame(maxWidth: .infinity, alignment: .center).transition(.opacity)
                        }
                    }
                    .frame(width: 330)
                    .padding(.top, 40)
                }
                .padding(.horizontal, 24).padding(.bottom, 20).padding(.top, 2)
            }
            .frame(width: 980, height: 722, alignment: .top)   // full window incl. the hidden title bar strip
            .ignoresSafeArea(edges: .top)
        }
        .frame(width: 980, height: 690)                       // window adds the 32 pt title-bar inset on top
        .animation(.easeInOut(duration: 0.2), value: launcher.message)
    }

    private var hero: some View {
        ZStack(alignment: .bottomLeading) {
            if let img = gameArt("library_hero.jpg") {
                Image(nsImage: img).resizable().aspectRatio(contentMode: .fill)
                    .frame(width: 980, height: 300).clipped()
                    .id(launcher.artVersion)
            } else {
                // before TF2 is installed: a TF2-coloured backdrop instead of Valve art
                LinearGradient(colors: [Color(red: 0.42, green: 0.20, blue: 0.13), .tfOrange.opacity(0.85), Color(red: 0.25, green: 0.30, blue: 0.36)],
                               startPoint: .topLeading, endPoint: .bottomTrailing)
                    .frame(width: 980, height: 300)
            }
            LinearGradient(colors: [.clear, .tfDark.opacity(0.45), .tfDark], startPoint: .center, endPoint: .bottom)
            HStack(alignment: .bottom) {
                VStack(alignment: .leading, spacing: 4) {
                    if let logo = gameArt("logo.png") {
                        Image(nsImage: logo).resizable().aspectRatio(contentMode: .fit).frame(height: 56)
                            .shadow(color: .black.opacity(0.6), radius: 8, y: 3)
                    } else {
                        Text("TEAM FORTRESS 2").font(.system(size: 44, weight: .black)).foregroundStyle(Color.tfCream)
                            .shadow(color: .black.opacity(0.6), radius: 8, y: 3)
                    }
                    Text("TF2MT  ·  TUNED FOR APPLE SILICON")
                        .font(.build(16)).tracking(1.5).foregroundStyle(Color.tfOrangeLight)
                        .shadow(color: .black, radius: 3)
                }
                Spacer()
                Text("\(displayName)  ·  \(displayRefreshHz) Hz")
                    .font(.secondary(13)).foregroundStyle(Color.tfCream.opacity(0.75)).shadow(color: .black, radius: 3)
            }
            .padding(.horizontal, 28).padding(.bottom, 12)
        }
        .frame(height: 300)
        .ignoresSafeArea(edges: .top)
    }

    private var settings: some View {
        Card(title: "") {
            OptionRow(title: "Native Metal Renderer",
                      detail: "tf2mt's Direct3D 9 → Metal renderer instead of DXVK.",
                      isOn: $metalRenderer)
            Divider().overlay(Color.white.opacity(0.06))
            HStack {
                RowText(title: "Graphics preset",
                        detail: launcher.comfigInstalled ? "mastercomfig profile. Applies on next launch."
                                                         : "mastercomfig, downloaded on first launch.")
                // no preset yet: shown as Low, which the first launch installs (Launcher.play)
                Picker("", selection: Binding(get: { launcher.comfigPreset == "none" ? "low" : launcher.comfigPreset },
                                              set: { launcher.setComfig($0) })) {
                    if launcher.comfigPreset != "none" && !Self.presets.contains(launcher.comfigPreset) {
                        Text(launcher.comfigPreset.capitalized).tag(launcher.comfigPreset)
                    }
                    ForEach(Self.presets, id: \.self) { p in
                        Text(p == "low" ? "Low (recommended, competitive)" : p.capitalized).tag(p)
                    }
                }
                .labelsHidden().frame(width: 250)
            }
            Divider().overlay(Color.white.opacity(0.06))
            HStack {
                RowText(title: "Frame pacing",
                        detail: Pacing(raw: pacingRaw).capFPS != nil ? "Caps pace unevenly under Wine; vsync is smoother."
                                                                    : "Vsync: no tearing. Uncapped: lowest latency.")
                Picker("", selection: pacing) {
                    ForEach(Pacing.options(displayHz: displayRefreshHz)) { p in
                        Text(p.label(displayHz: displayRefreshHz)).tag(p)
                    }
                }
                .labelsHidden().frame(width: 250)
            }
            Divider().overlay(Color.white.opacity(0.06))
            OptionRow(title: "Smooth mouse fix",
                      detail: launcher.mouseFix == .unknown ? "Run Setup first."
                                                            : "Aim updates every frame, at any refresh rate.",
                      isOn: Binding(get: { launcher.mouseFix == .applied }, set: { launcher.setMouseFix($0) }))
                .disabled(launcher.mouseFix == .unknown)
            Divider().overlay(Color.white.opacity(0.06))
            OptionRow(title: "Friends offline while playing",
                      detail: "Avoids menu stutter. Restored on exit.",
                      isOn: $friendsOffline)
            Divider().overlay(Color.white.opacity(0.06))
            VStack(alignment: .leading, spacing: 6) {
                Text("Extra launch options").font(.secondary(15)).foregroundStyle(Color.tfCream)
                TextField("e.g. +exec myconfig  -console", text: $extraArgs)
                    .textFieldStyle(.plain).font(.system(size: 13, design: .monospaced))
                    .padding(8).background(Color.black.opacity(0.35), in: RoundedRectangle(cornerRadius: 7))
                    .foregroundStyle(Color.tfCream)
            }
        }
    }
    private static let presets = ["low", "medium", "high", "ultra"]

    private var debug: some View {
        Card(title: "") {
            OptionRow(title: "Metal performance HUD",
                      detail: "Apple's FPS and frame-time overlay.", isOn: $metalHUD)
            Divider().overlay(Color.white.opacity(0.06))
            OptionRow(title: "Record frame times (DXVK)",
                      detail: metalRenderer ? "The Metal renderer always records." : "Logs to ~/Games/tf2/logs/dxvk/.",
                      isOn: $frameLog)
                .disabled(metalRenderer)
        }
    }

    private var setup: some View {
        Card(title: "") {
            Text("tf2mt brings its own Wine runtime; Steam and TF2 are installed from Valve, into your own folder. Nothing from Valve is redistributed.")
                .font(.system(size: 12)).foregroundStyle(Color.tfCream.opacity(0.7))
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 10) {
                Button(launcher.setupRunning ? "Setting up…" : "Run setup") { launcher.runSetupInteractive() }
                    .buttonStyle(SmallButtonStyle(prominent: true)).disabled(launcher.setupRunning)
                Button("Log in to Steam") { launcher.loginSteam() }
                    .buttonStyle(SmallButtonStyle()).disabled(!exists(Paths.steamExe))
                Button("Install TF2") { launcher.installTF2() }
                    .buttonStyle(SmallButtonStyle()).disabled(launcher.steam == .stopped)
            }
            if !launcher.setupLog.isEmpty {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(Array(launcher.setupLog.suffix(14).enumerated()), id: \.offset) { _, line in
                        Text(line).font(.system(size: 11, design: .monospaced))
                            .foregroundStyle(line.hasPrefix("FAIL") ? Color.tfRed : line.hasPrefix("==>") ? Color.tfOrangeLight : Color.tfCream.opacity(0.75))
                    }
                }
                .padding(10).frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.black.opacity(0.35), in: RoundedRectangle(cornerRadius: 7))
            }
            ForEach(Array(launcher.checks.enumerated()), id: \.element.id) { i, c in
                HStack(alignment: .top, spacing: 12) {
                    Image(systemName: c.ok ? "checkmark.circle.fill" : "\(i + 1).circle")
                        .font(.system(size: 17)).foregroundStyle(c.ok ? Color.tfGreen : Color.tfOrangeLight)
                        .frame(width: 22)
                    RowText(title: c.title, detail: c.ok ? "Ready." : c.fix)
                }
            }
            HStack {
                RowText(title: "Game folder", detail: Paths.game)
                Button("Change…") { launcher.chooseGameFolder() }.buttonStyle(SmallButtonStyle()).frame(width: 90)
            }
            Button("Re-check") { launcher.refreshMouseFix(); launcher.refreshComfig(); launcher.refresh() }.buttonStyle(SmallButtonStyle())
        }
    }

    private var status: some View {
        Card(title: "Status") {
            StatusRow(label: "Steam",
                      value: launcher.steam == .online ? "Online" : launcher.steam == .starting ? "Logging in…" : "Not running",
                      color: launcher.steam == .online ? .green : launcher.steam == .starting ? .yellow : .gray)
            StatusRow(label: "Team Fortress 2",
                      value: launcher.tf2Running ? "Running" : launcher.launching ? "Launching…" : "Not running",
                      color: launcher.tf2Running ? .green : launcher.launching ? .yellow : .gray)
            StatusRow(label: "Mouse fix",
                      value: launcher.mouseFix == .applied ? "Active" : launcher.mouseFix == .original ? "Off" : "Unavailable",
                      color: launcher.mouseFix == .applied ? .green : launcher.mouseFix == .original ? .tfRed : .gray)
        }
    }

    private var playButton: some View {
        let canPlay = !launcher.tf2Running && !launcher.launching && launcher.readyToPlay
        return Button {
            launcher.play(pacing: Pacing(raw: pacingRaw), hud: metalHUD, friendsOffline: friendsOffline,
                          metalRenderer: metalRenderer, frameLog: frameLog, extraArgs: extraArgs)
        } label: {
            HStack(spacing: 12) {
                if launcher.launching { ProgressView().controlSize(.small).tint(.white) }
                Text(launcher.tf2Running ? "RUNNING" : launcher.launching ? "LAUNCHING" : launcher.readyToPlay ? "PLAY" : "SET UP")
            }
        }
        .buttonStyle(PlayButtonStyle(enabled: canPlay))
        .disabled(!canPlay)
        .keyboardShortcut(.defaultAction)
    }
}

@main
struct TF2LauncherApp: App {
    var body: some Scene {
        WindowGroup("tf2mt") {
            ContentView().preferredColorScheme(.dark)
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentSize)
    }
}
