import Foundation
import CoreML

func deviceName(_ d: MLComputeDevice?) -> String {
    guard let d else { return "none" }
    // The concrete types are public classes (MLCPUComputeDevice, MLGPUComputeDevice, MLNeuralEngineComputeDevice);
    // describing type(of:) only prints the shared superclass, which is why an earlier version reported "MLComputeDevice"
    // for every op and told us nothing.
    if d is MLNeuralEngineComputeDevice { return "ane" }
    if d is MLGPUComputeDevice { return "gpu" }
    if d is MLCPUComputeDevice { return "cpu" }
    return "?"
}

let args = CommandLine.arguments
guard args.count > 1 else { print("usage: plan <model.mlmodelc> [cpu_ne|all]"); exit(2) }
let cfg = MLModelConfiguration()
cfg.computeUnits = (args.count > 2 && args[2] == "all") ? .all : .cpuAndNeuralEngine
let url = URL(fileURLWithPath: args[1])
let sem = DispatchSemaphore(value: 0)

Task {
    do {
        let plan = try await MLComputePlan.load(contentsOf: url, configuration: cfg)
        guard case .program(let program) = plan.modelStructure,
              let main = program.functions["main"] else { print("no program/main"); sem.signal(); return }
        var byDevice: [String: Int] = [:]
        var byType: [String: [String: Int]] = [:]
        var costSum = 0.0
        var costCount = 0
        func walk(_ block: MLModelStructure.Program.Block) {
            for op in block.operations {
                let dev = deviceName(plan.deviceUsage(for: op)?.preferred)
                byDevice[dev, default: 0] += 1
                byType[op.operatorName, default: [:]][dev, default: 0] += 1
                if let c = plan.estimatedCost(of: op) { costSum += c.weight; costCount += 1 }
                for nested in op.blocks { walk(nested) }
            }
        }
        walk(main.block)
        print("model: \(url.lastPathComponent)  units=\(cfg.computeUnits == .all ? "all" : "cpu_ne")")
        print("  ops by preferred device: \(byDevice.sorted { $0.value > $1.value })")
        for (t, d) in byType.sorted(by: { $0.value.values.reduce(0,+) > $1.value.values.reduce(0,+) }).prefix(8) {
            print("    \(t): \(d.sorted { $0.value > $1.value })")
        }
        print("  cost: sum=\(String(format: "%.2f", costSum)) over \(costCount) ops")
    } catch {
        print("error: \(error)")
    }
    sem.signal()
}
sem.wait()
