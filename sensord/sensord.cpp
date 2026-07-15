// SPDX-License-Identifier: Apache-2.0
/*
 * sensord.cpp - Minimal OpenBMC-style sensor daemon.
 *
 * Pipeline:
 *   hwmon sysfs (tempN_input, milli-degC)
 *     -> periodic poll (boost::asio steady_timer)
 *     -> D-Bus objects at /xyz/openbmc_project/sensors/temperature/<name>
 *        implementing:
 *          xyz.openbmc_project.Sensor.Value
 *          xyz.openbmc_project.Sensor.Threshold.Warning
 *          xyz.openbmc_project.Sensor.Threshold.Critical
 *
 * Design follows openbmc/dbus-sensors conventions:
 *   - sdbusplus::asio::object_server, properties registered dynamically
 *     (no generated server bindings; interface set is data-driven)
 *   - PropertiesChanged is emitted only when a value actually changes
 *     (set_property() is a no-op signal-wise for identical values)
 *   - threshold alarms use hysteresis on deassert to avoid flapping
 *
 * Usage:
 *   sensord [--user] [--interval <ms>] [--hwmon-root <path>]
 *     --user        connect to the session bus (development without a
 *                   system-bus policy file)
 *     --hwmon-root  scan this directory instead of /sys/class/hwmon
 *                   (lets the demo run against a fake sysfs tree)
 */

 #include <boost/asio/io_context.hpp>
 #include <boost/asio/steady_timer.hpp>
 #include <sdbusplus/asio/connection.hpp>
 #include <sdbusplus/asio/object_server.hpp>
 
 #include <chrono>
 #include <cmath>
 #include <filesystem>
 #include <fstream>
 #include <iostream>
 #include <memory>
 #include <optional>
 #include <string>
 #include <vector>
 
 namespace fs = std::filesystem;
 
 // ---------------------------------------------------------------------------
 // Constants (OpenBMC D-Bus naming conventions)
 // ---------------------------------------------------------------------------
 
 static constexpr const char* busName = "xyz.openbmc_project.VirtualSensor";
 static constexpr const char* sensorRoot = "/xyz/openbmc_project/sensors";
 static constexpr const char* valueIfaceName =
     "xyz.openbmc_project.Sensor.Value";
 static constexpr const char* warningIfaceName =
     "xyz.openbmc_project.Sensor.Threshold.Warning";
 static constexpr const char* criticalIfaceName =
     "xyz.openbmc_project.Sensor.Threshold.Critical";
 static constexpr const char* unitDegreesC =
     "xyz.openbmc_project.Sensor.Value.Unit.DegreesC";
 
 static constexpr double hysteresis = 2.0;      // degC, applied on deassert
 static constexpr double defaultCritical = 85.0; // degC, if no tempN_max
 static constexpr double warningMargin = 10.0;   // Warning = Critical - margin
 
 // ---------------------------------------------------------------------------
 // Small sysfs helpers
 // ---------------------------------------------------------------------------
 
 // Read a whole sysfs file as a long. hwmon temperature files are in
 // milli-degree Celsius by ABI (Documentation/hwmon/sysfs-interface.rst).
 static std::optional<long> readSysfsLong(const fs::path& p)
 {
     std::ifstream f(p);
     long v;
     if (f >> v)
     {
         return v;
     }
     return std::nullopt;
 }
 
 static std::string readSysfsString(const fs::path& p)
 {
     std::ifstream f(p);
     std::string s;
     std::getline(f, s);
     return s;
 }
 
 // Object path elements may only contain [A-Za-z0-9_].
 static std::string sanitize(std::string s)
 {
     for (char& c : s)
     {
         if (!std::isalnum(static_cast<unsigned char>(c)))
         {
             c = '_';
         }
     }
     return s;
 }
 
 // ---------------------------------------------------------------------------
 // One threshold level (Warning or Critical) with assert/deassert hysteresis
 // ---------------------------------------------------------------------------
 
 struct Threshold
 {
     double high = 0.0;          // threshold value, degC
     bool asserted = false;      // current alarm state
     std::string alarmProperty;  // "WarningAlarmHigh" / "CriticalAlarmHigh"
     std::shared_ptr<sdbusplus::asio::dbus_interface> iface;
 
     // Returns true when the alarm state changed.
     // Assert at  value >= high
     // Deassert at value <  high - hysteresis   (NOT at high itself)
     // A sensor hovering around the threshold would otherwise emit an
     // assert/deassert pair on every poll cycle.
     bool evaluate(double value)
     {
         bool next = asserted;
         if (!asserted && value >= high)
         {
             next = true;
         }
         else if (asserted && value < high - hysteresis)
         {
             next = false;
         }
         if (next == asserted)
         {
             return false;
         }
         asserted = next;
         iface->set_property(alarmProperty, asserted);
         return true;
     }
 };
 
 // ---------------------------------------------------------------------------
 // Sensor: one hwmon temperature channel mirrored onto D-Bus
 // ---------------------------------------------------------------------------
 
 class Sensor
 {
   public:
     Sensor(sdbusplus::asio::object_server& server, const std::string& name,
            fs::path inputPath, double criticalHigh) :
         name_(name), inputPath_(std::move(inputPath))
     {
         std::string path =
             std::string(sensorRoot) + "/temperature/" + name_;
 
         // --- xyz.openbmc_project.Sensor.Value ---
         valueIface_ = server.add_interface(path, valueIfaceName);
         valueIface_->register_property("Value", value_);
         valueIface_->register_property("Unit", std::string(unitDegreesC));
         valueIface_->register_property("MaxValue", 125.0);
         valueIface_->register_property("MinValue", -40.0);
         valueIface_->initialize();
 
         // --- Threshold.Warning / Threshold.Critical ---
         critical_.high = criticalHigh;
         critical_.alarmProperty = "CriticalAlarmHigh";
         critical_.iface = server.add_interface(path, criticalIfaceName);
         critical_.iface->register_property("CriticalHigh", critical_.high);
         critical_.iface->register_property("CriticalAlarmHigh", false);
         critical_.iface->initialize();
 
         warning_.high = criticalHigh - warningMargin;
         warning_.alarmProperty = "WarningAlarmHigh";
         warning_.iface = server.add_interface(path, warningIfaceName);
         warning_.iface->register_property("WarningHigh", warning_.high);
         warning_.iface->register_property("WarningAlarmHigh", false);
         warning_.iface->initialize();
 
         std::cout << "sensor " << name_ << " at " << path << " (warning "
                   << warning_.high << ", critical " << critical_.high
                   << " degC)\n";
     }
 
     void poll()
     {
         auto raw = readSysfsLong(inputPath_);
         if (!raw)
         {
             // Real dbus-sensors would flip OperationalStatus.Functional
             // here; out of scope for this milestone.
             std::cerr << name_ << ": read failed\n";
             return;
         }
 
         double v = static_cast<double>(*raw) / 1000.0; // milli-degC -> degC
         if (v != value_)
         {
             value_ = v;
             // set_property emits PropertiesChanged only because the value
             // differs; identical writes are filtered inside sdbusplus.
             valueIface_->set_property("Value", value_);
         }
 
         if (warning_.evaluate(value_))
         {
             logTransition(warning_);
         }
         if (critical_.evaluate(value_))
         {
             logTransition(critical_);
         }
     }
 
   private:
     void logTransition(const Threshold& t) const
     {
         std::cout << name_ << ": " << t.alarmProperty << " -> "
                   << (t.asserted ? "ASSERT" : "DEASSERT") << " (value "
                   << value_ << ", threshold " << t.high << ")" << std::endl;
     }
 
     std::string name_;
     fs::path inputPath_;
     double value_ = std::nan("");
     std::shared_ptr<sdbusplus::asio::dbus_interface> valueIface_;
     Threshold warning_;
     Threshold critical_;
 };
 
 // ---------------------------------------------------------------------------
 // hwmon discovery: every tempN_input under every chip becomes a sensor
 // ---------------------------------------------------------------------------
 
 static std::vector<std::unique_ptr<Sensor>>
     discoverSensors(sdbusplus::asio::object_server& server,
                     const fs::path& hwmonRoot)
 {
     std::vector<std::unique_ptr<Sensor>> sensors;
 
     if (!fs::exists(hwmonRoot))
     {
         return sensors;
     }
 
     for (const auto& chip : fs::directory_iterator(hwmonRoot))
     {
         std::string chipName = readSysfsString(chip.path() / "name");
         if (chipName.empty())
         {
             continue;
         }
 
         for (const auto& entry : fs::directory_iterator(chip.path()))
         {
             const std::string file = entry.path().filename().string();
             if (!file.starts_with("temp") || !file.ends_with("_input"))
             {
                 continue;
             }
             // "temp1_input" -> channel prefix "temp1"
             const std::string channel =
                 file.substr(0, file.size() - std::string("_input").size());
 
             // Reuse the kernel-side limit when the chip provides one:
             // tempN_max (milli-degC) becomes CriticalHigh.
             double critical = defaultCritical;
             if (auto max =
                     readSysfsLong(chip.path() / (channel + "_max")))
             {
                 critical = static_cast<double>(*max) / 1000.0;
             }
 
             sensors.push_back(std::make_unique<Sensor>(
                 server, sanitize(chipName + "_" + channel), entry.path(),
                 critical));
         }
     }
     return sensors;
 }
 
 // ---------------------------------------------------------------------------
 // main: bus setup + poll loop
 // ---------------------------------------------------------------------------
 
 int main(int argc, char** argv)
 {
     bool userBus = false;
     fs::path hwmonRoot = "/sys/class/hwmon";
     auto interval = std::chrono::milliseconds(1000);
 
     for (int i = 1; i < argc; ++i)
     {
         std::string a = argv[i];
         if (a == "--user")
         {
             userBus = true;
         }
         else if (a == "--hwmon-root" && i + 1 < argc)
         {
             hwmonRoot = argv[++i];
         }
         else if (a == "--interval" && i + 1 < argc)
         {
             interval = std::chrono::milliseconds(std::stol(argv[++i]));
         }
         else
         {
             std::cerr << "usage: sensord [--user] [--interval <ms>]"
                          " [--hwmon-root <path>]\n";
             return 1;
         }
     }
 
     boost::asio::io_context io;
 
     // System bus is where real BMC services live; the session bus is a
     // development convenience that needs no policy file.
     sd_bus* bus = nullptr;
     int r = userBus ? sd_bus_open_user(&bus) : sd_bus_open_system(&bus);
     if (r < 0)
     {
         std::cerr << "failed to connect to " << (userBus ? "user" : "system")
                   << " bus: " << strerror(-r) << "\n";
         return 1;
     }
     auto conn = std::make_shared<sdbusplus::asio::connection>(io, bus);
 
     // ObjectManager at the sensor root: consumers (e.g. bmcweb) call
     // GetManagedObjects here and receive InterfacesAdded for new sensors.
     sdbusplus::asio::object_server server(conn);
     server.add_manager(sensorRoot);
 
     auto sensors = discoverSensors(server, hwmonRoot);
     if (sensors.empty())
     {
         std::cerr << "no hwmon temperature inputs found under " << hwmonRoot
                   << "\n";
         return 1;
     }
 
     // Claim the well-known name only after objects exist, so a consumer
     // that resolves the name never observes an empty object tree.
     conn->request_name(busName);
 
     boost::asio::steady_timer timer(io);
     std::function<void(const boost::system::error_code&)> tick =
         [&](const boost::system::error_code& ec) {
             if (ec)
             {
                 return; // timer cancelled -> shutting down
             }
             for (auto& s : sensors)
             {
                 s->poll();
             }
             timer.expires_after(interval);
             timer.async_wait(tick);
         };
     timer.expires_after(interval);
     timer.async_wait(tick);
 
     std::cout << "sensord up: " << sensors.size() << " sensor(s), polling "
               << interval.count() << " ms" << std::endl;
     io.run();
     return 0;
 }
 