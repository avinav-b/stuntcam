// SPDX-License-Identifier: Apache-2.0
// The canary is a throwaway app. It drives the whole capture flow through AVFoundation's own consumers and exits with a
// verdict (Report.swift): exit 0 is PASS, 1 is FAIL, 4 is INCONCLUSIVE and 3 is its own watchdog. A crash leaves no
// RESULT line.
//
// This file starts the app. It opens one window, then runs Canary.run() (Canary.swift) on a thread of its own, with a
// watchdog that ends the run when the budget (Config.swift) runs out. The app adopts UIScene, as the iOS 27 SDK
// expects.
import UIKit
import AVFoundation
import ImageIO

// The check lines' clock starts here: top-level code in main.swift runs at launch, before the app starts.
let launchTime = Date()
let config = Config.fromEnvironment()

let canary = Canary()
final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
  var window: UIWindow?
  func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
    guard let windowScene = scene as? UIWindowScene else { return }
    let sceneWindow = UIWindow(windowScene: windowScene)
    sceneWindow.rootViewController = UIViewController()
    sceneWindow.rootViewController!.view.backgroundColor = .black
    sceneWindow.makeKeyAndVisible()
    window = sceneWindow
    if canaryStarted { return }
    canaryStarted = true
    canary.window = sceneWindow
    DispatchQueue.global().asyncAfter(deadline: .now() + config.budget) { watchdogExit() }
    log("start budget=\(config.budget)s")
    Thread { canary.run() }.start()
  }
}

final class AppDelegate: UIResponder, UIApplicationDelegate {
  func application(_ application: UIApplication,
                   didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
    true
  }

  func application(_ application: UIApplication, configurationForConnecting connectingSession: UISceneSession,
                   options: UIScene.ConnectionOptions) -> UISceneConfiguration {
    let configuration = UISceneConfiguration(name: "Default", sessionRole: connectingSession.role)
    configuration.delegateClass = SceneDelegate.self
    return configuration
  }
}

var canaryStarted = false
UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(AppDelegate.self))
