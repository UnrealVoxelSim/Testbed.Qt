#include "UnrealVoxelSim/Profiling/Api/Macros.h"
#include "UnrealVoxelSim/Profiling/Api/NullRecorder.h"
#include "UnrealVoxelSim/Testbed/World.h"
#if defined(UNREALVOXELSIM_PROFILING_ENABLED)
#include "UnrealVoxelSim/Profiling/Tracy/Recorder.h"
#endif

#include <algorithm>
#include <array>
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
#include <unordered_map>
#include <vector>

namespace
{
namespace ProfilingApi = UnrealVoxelSim::Profiling::Api;
namespace SimulationApi = UnrealVoxelSim::Simulation::Api;

class TimingRecorder final : public ProfilingApi::IRecorder
{
public:
    struct ZoneStats final
    {
        std::uint64_t Nanoseconds{};
        std::size_t Count{};
    };

    explicit TimingRecorder(ProfilingApi::IRecorder& underlying) noexcept : m_Underlying(underlying) {}

    [[nodiscard]] ProfilingApi::ZoneToken BeginZone(const ProfilingApi::SourceLocation& location) noexcept override
    {
        try
        {
            const auto token = m_Underlying.BeginZone(location);
            m_Active.push_back({location.Name, std::chrono::steady_clock::now(), token});
            return {static_cast<std::uint64_t>(m_Active.size())};
        }
        catch (...)
        {
            return {};
        }
    }

    void EndZone(const ProfilingApi::ZoneToken token) noexcept override
    {
        if (token.Payload == 0 || token.Payload != m_Active.size()) return;
        const auto entry = m_Active.back();
        m_Active.pop_back();
        m_Underlying.EndZone(entry.Token);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - entry.Start).count();
        auto& stats = m_Stats[entry.Name];
        stats.Nanoseconds += static_cast<std::uint64_t>(elapsed);
        ++stats.Count;
    }

    void MarkFrame(const char* name) noexcept override { m_Underlying.MarkFrame(name); }
    void Plot(const char* name, double value) noexcept override { m_Underlying.Plot(name, value); }
    void Message(std::string_view message) noexcept override { m_Underlying.Message(message); }
    void SetThreadName(const char* name) noexcept override { m_Underlying.SetThreadName(name); }
    [[nodiscard]] bool IsConnected() const noexcept override { return m_Underlying.IsConnected(); }

    void Reset() noexcept
    {
        m_Stats.clear();
        m_Active.clear();
    }

    [[nodiscard]] const std::unordered_map<std::string, ZoneStats>& Stats() const noexcept { return m_Stats; }

private:
    struct ActiveZone final
    {
        const char* Name{};
        std::chrono::steady_clock::time_point Start;
        ProfilingApi::ZoneToken Token;
    };

    ProfilingApi::IRecorder& m_Underlying;
    std::vector<ActiveZone> m_Active;
    std::unordered_map<std::string, ZoneStats> m_Stats;
};

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
    std::size_t NavigationStarts{};
    std::array<std::size_t, 6> NavigationCounts{};
};

[[nodiscard]] Result MeasurePopulation(const std::string_view worldId, const std::size_t population,
                                       const Options &options, TimingRecorder &profiling)
{
    UNREALVOXELSIM_PROFILE_ZONE(profiling, "Headless population run");
    auto world = UnrealVoxelSim::Testbed::WorldCatalog::Create(worldId, profiling);
    world->SetTargetPopulation(population);
    if (world->Pawns().size() != population)
        throw std::invalid_argument{"Requested population exceeds the selected world's spawn capacity."};

    if (const auto warmup = world->Stepper().Step(SimulationApi::TickCount{options.WarmupSteps}); !warmup)
        throw std::overflow_error{"Simulation tick overflow during warm-up."};

    const auto measurementStart = world->Stats();
    profiling.Reset();

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
    const auto measurementEnd = world->Stats();
    std::vector<std::pair<std::string, TimingRecorder::ZoneStats>> zones;
    for (const auto& entry : profiling.Stats()) zones.push_back(entry);
    std::ranges::sort(zones, [](const auto& left, const auto& right)
                      { return left.second.Nanoseconds > right.second.Nanoseconds; });
    std::cout << "phases,population,"
                 "name,total_ms_per_step,calls_per_step\n";
    for (const auto& [name, stats] : zones)
        std::cout << "phase," << population << ',' << name << ','
                  << (static_cast<double>(stats.Nanoseconds) /
                      (options.StepsPerSample * options.Samples) / 1'000'000.0) << ','
                  << (static_cast<double>(stats.Count) / (options.StepsPerSample * options.Samples)) << '\n';
    return {rates.front(), rates[rates.size() / 2], rates.back(),
            measurementEnd.NavigationStarts - measurementStart.NavigationStarts,
            measurementEnd.NavigationCounts};
}
}

int main(const int argc, char *argv[])
{
    try
    {
        const auto options = ParseOptions({argv, static_cast<std::size_t>(argc)});
#if defined(UNREALVOXELSIM_PROFILING_ENABLED)
        UnrealVoxelSim::Profiling::Tracy::Recorder baseProfiling;
        constexpr auto profilingEnabled = true;
#else
        UnrealVoxelSim::Profiling::Api::NullRecorder baseProfiling;
        constexpr auto profilingEnabled = false;
#endif
        TimingRecorder profiling{baseProfiling};
        UNREALVOXELSIM_PROFILE_THREAD(profiling, "Headless simulation");
        std::cout << "# world=" << options.World << ",profiling_enabled=" << (profilingEnabled ? 1 : 0)
                  << ",profiling_connected=" << (profiling.IsConnected() ? 1 : 0)
                  << ",hardware_threads=" << std::thread::hardware_concurrency() << '\n';
        std::cout << "entities,warmup_steps,steps_per_sample,samples,min_steps_per_second,median_steps_per_second,"
                     "max_steps_per_second,navigation_starts,planning,following,replanning,arrived,unreachable,"
                     "cancelled\n";
        std::cout << std::fixed << std::setprecision(2);
        for (const auto population : options.Populations)
        {
            const auto result = MeasurePopulation(options.World, population, options, profiling);
            std::cout << population << ',' << options.WarmupSteps << ',' << options.StepsPerSample << ','
                      << options.Samples << ',' << result.MinimumStepsPerSecond << ','
                      << result.MedianStepsPerSecond << ',' << result.MaximumStepsPerSecond << ','
                      << result.NavigationStarts;
            for (const auto count : result.NavigationCounts)
                std::cout << ',' << count;
            std::cout << '\n';
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
