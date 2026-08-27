#include "UnrealVoxelSim/Profiling/Api/Macros.h"
#include "UnrealVoxelSim/Profiling/Api/NullRecorder.h"
#include "UnrealVoxelSim/Testbed/World.h"
#if defined(UNREALVOXELSIM_PROFILING_ENABLED)
#include "UnrealVoxelSim/Profiling/Tracy/Recorder.h"
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
namespace ProfilingApi = UnrealVoxelSim::Profiling::Api;
namespace SimulationApi = UnrealVoxelSim::Simulation::Api;

struct Options final
{
    std::string World{"stress"};
    std::vector<std::size_t> Populations{0, 100, 1'000, 5'000, 10'000};
    std::uint64_t WarmupSteps{200};
    std::uint64_t StepsPerSample{200};
    std::size_t Samples{3};
};

[[nodiscard]] std::size_t ParseSize(const std::string_view text, const std::string_view option)
{
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument{std::string{option} + " requires a non-negative integer."};
    return static_cast<std::size_t>(value);
}

[[nodiscard]] std::vector<std::size_t> ParsePopulations(const std::string_view text)
{
    std::vector<std::size_t> result;
    for (std::size_t begin = 0; begin <= text.size();)
    {
        const auto separator = text.find(',', begin);
        const auto end = separator == std::string_view::npos ? text.size() : separator;
        if (end == begin) throw std::invalid_argument{"--entities contains an empty population."};
        result.push_back(ParseSize(text.substr(begin, end - begin), "--entities"));
        if (separator == std::string_view::npos) break;
        begin = separator + 1;
    }
    if (result.empty()) throw std::invalid_argument{"--entities requires at least one population."};
    return result;
}

[[nodiscard]] Options ParseOptions(const std::span<char *> arguments)
{
    Options options;
    for (std::size_t index = 1; index < arguments.size(); ++index)
    {
        const std::string_view argument{arguments[index]};
        if (argument == "--help")
        {
            std::cout << "Usage: UnrealVoxelSim.Testbed.Qt.Headless [--world stress] "
                         "[--entities 0,100,1000,5000,10000] [--warmup-steps 200] "
                         "[--measurement-steps 200] [--samples 3]\n";
            std::exit(0);
        }
        if (index + 1 >= arguments.size()) throw std::invalid_argument{std::string{argument} + " requires a value."};
        const std::string_view value{arguments[++index]};
        if (argument == "--world")
            options.World = value;
        else if (argument == "--entities")
            options.Populations = ParsePopulations(value);
        else if (argument == "--warmup-steps")
            options.WarmupSteps = ParseSize(value, argument);
        else if (argument == "--measurement-steps")
            options.StepsPerSample = ParseSize(value, argument);
        else if (argument == "--samples")
            options.Samples = ParseSize(value, argument);
        else
            throw std::invalid_argument{"Unknown option: " + std::string{argument}};
    }
    if (options.StepsPerSample == 0) throw std::invalid_argument{"--measurement-steps must be positive."};
    if (options.Samples == 0) throw std::invalid_argument{"--samples must be positive."};
    return options;
}

struct Result final
{
    double MinimumStepsPerSecond{};
    double MedianStepsPerSecond{};
    double MaximumStepsPerSecond{};
};

[[nodiscard]] Result MeasurePopulation(const std::string_view worldId, const std::size_t population,
                                       const Options &options, ProfilingApi::IRecorder &profiling)
{
    UNREALVOXELSIM_PROFILE_ZONE(profiling, "Headless population run");
    auto world = UnrealVoxelSim::Testbed::WorldCatalog::Create(worldId, profiling);
    world->SetTargetPopulation(population);
    if (world->Pawns().size() != population)
        throw std::invalid_argument{"Requested population exceeds the selected world's spawn capacity."};

    if (const auto warmup = world->Stepper().Step(SimulationApi::TickCount{options.WarmupSteps}); !warmup)
        throw std::overflow_error{"Simulation tick overflow during warm-up."};

    std::vector<double> rates;
    rates.reserve(options.Samples);
    for (std::size_t sample = 0; sample < options.Samples; ++sample)
    {
        const auto start = std::chrono::steady_clock::now();
        {
            UNREALVOXELSIM_PROFILE_ZONE(profiling, "Headless measured sample");
            if (const auto measured = world->Stepper().Step(SimulationApi::TickCount{options.StepsPerSample}); !measured)
                throw std::overflow_error{"Simulation tick overflow during measurement."};
        }
        const auto elapsed = std::chrono::duration<double>{std::chrono::steady_clock::now() - start}.count();
        rates.push_back(static_cast<double>(options.StepsPerSample) / elapsed);
    }
    std::ranges::sort(rates);
    UNREALVOXELSIM_PROFILE_PLOT(profiling, "Headless measured population", population);
    UNREALVOXELSIM_PROFILE_PLOT(profiling, "Headless maximum steps per second", rates.back());
    return {rates.front(), rates[rates.size() / 2], rates.back()};
}
}

int main(const int argc, char *argv[])
{
    try
    {
        const auto options = ParseOptions({argv, static_cast<std::size_t>(argc)});
#if defined(UNREALVOXELSIM_PROFILING_ENABLED)
        UnrealVoxelSim::Profiling::Tracy::Recorder profiling;
        constexpr auto profilingEnabled = true;
#else
        UnrealVoxelSim::Profiling::Api::NullRecorder profiling;
        constexpr auto profilingEnabled = false;
#endif
        UNREALVOXELSIM_PROFILE_THREAD(profiling, "Headless simulation");
        std::cout << "# world=" << options.World << ",profiling_enabled=" << (profilingEnabled ? 1 : 0)
                  << ",profiling_connected=" << (profiling.IsConnected() ? 1 : 0)
                  << ",hardware_threads=" << std::thread::hardware_concurrency() << '\n';
        std::cout << "entities,warmup_steps,steps_per_sample,samples,min_steps_per_second,median_steps_per_second,"
                     "max_steps_per_second\n";
        std::cout << std::fixed << std::setprecision(2);
        for (const auto population : options.Populations)
        {
            const auto result = MeasurePopulation(options.World, population, options, profiling);
            std::cout << population << ',' << options.WarmupSteps << ',' << options.StepsPerSample << ','
                      << options.Samples << ',' << result.MinimumStepsPerSecond << ','
                      << result.MedianStepsPerSecond << ',' << result.MaximumStepsPerSecond << '\n';
            std::cout.flush();
        }
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "Headless simulation failed: " << exception.what() << '\n';
        return 1;
    }
}
