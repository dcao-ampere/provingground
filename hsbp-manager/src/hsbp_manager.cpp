/*
// Copyright (c) 2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "utils.hpp"

#include <boost/algorithm/string/replace.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/container/flat_set.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/bus/match.hpp>
#include <string>
#include <utility>

extern "C" {
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
}

constexpr const char* configType =
    "xyz.openbmc_project.Configuration.Intel_HSBP_CPLD";
constexpr const char* busName = "xyz.openbmc_project.HsbpManager";

constexpr size_t scanRateSeconds = 5;
constexpr size_t maxDrives = 8; // only 1 byte alloted

boost::asio::io_context io;
auto conn = std::make_shared<sdbusplus::asio::connection>(io);
sdbusplus::asio::object_server objServer(conn);

static std::string zeroPad(const uint8_t val)
{
    std::ostringstream version;
    version << std::setw(2) << std::setfill('0') << static_cast<size_t>(val);
    return version.str();
}

struct Mux
{
    Mux(size_t busIn, size_t addressIn, size_t channelsIn, size_t indexIn) :
        bus(busIn), address(addressIn), channels(channelsIn), index(indexIn)
    {
    }
    size_t bus;
    size_t address;
    size_t channels;
    size_t index;

    // to sort in the flat set
    bool operator<(const Mux& rhs) const
    {
        return index < rhs.index;
    }
};

enum class BlinkPattern : uint8_t
{
    off = 0x0,
    error = 0x2,
    terminate = 0x3
};

struct Led : std::enable_shared_from_this<Led>
{
    // led pattern addresses start at 0x10
    Led(const std::string& path, size_t index, int fd) :
        address(static_cast<uint8_t>(index + 0x10)), file(fd),
        ledInterface(objServer.add_interface(path, ledGroup::interface))
    {
        if (index >= maxDrives)
        {
            throw std::runtime_error("Invalid drive index");
        }

        if (!set(BlinkPattern::off))
        {
            std::cerr << "Cannot initialize LED " << path << "\n";
        }
    }

    // this has to be called outside the constructor for shared_from_this to
    // work
    void createInterface(void)
    {
        std::shared_ptr<Led> self = shared_from_this();

        ledInterface->register_property(
            ledGroup::asserted, false, [self](const bool req, bool& val) {
                if (req == val)
                {
                    return 1;
                }

                if (!isPowerOn())
                {
                    std::cerr << "Can't change blink state when power is off\n";
                    throw std::runtime_error(
                        "Can't change blink state when power is off");
                }
                BlinkPattern pattern =
                    req ? BlinkPattern::error : BlinkPattern::terminate;
                if (!self->set(pattern))
                {
                    std::cerr << "Can't change blink pattern\n";
                    throw std::runtime_error("Cannot set blink pattern");
                }
                val = req;
                return 1;
            });
        ledInterface->initialize();
    }

    virtual ~Led()
    {
        objServer.remove_interface(ledInterface);
    }

    bool set(BlinkPattern pattern)
    {
        int ret = i2c_smbus_write_byte_data(file, address,
                                            static_cast<uint8_t>(pattern));
        return ret >= 0;
    }

    uint8_t address;
    int file;
    std::shared_ptr<sdbusplus::asio::dbus_interface> ledInterface;
};

struct Drive
{
    Drive(size_t driveIndex, bool isPresent, bool isOperational, bool nvme,
          bool rebuilding) :
        isNvme(nvme)
    {
        constexpr const char* basePath =
            "/xyz/openbmc_project/inventory/item/drive/Drive_";
        itemIface = objServer.add_interface(
            basePath + std::to_string(driveIndex), inventory::interface);
        itemIface->register_property("Present", isPresent);
        itemIface->register_property("PrettyName",
                                     "Drive " + std::to_string(driveIndex));
        itemIface->initialize();
        operationalIface = objServer.add_interface(
            itemIface->get_object_path(),
            "xyz.openbmc_project.State.Decorator.OperationalStatus");
        operationalIface->register_property("Functional", isOperational);
        operationalIface->initialize();
        rebuildingIface = objServer.add_interface(
            itemIface->get_object_path(), "xyz.openbmc_project.State.Drive");
        rebuildingIface->register_property("Rebuilding", rebuilding);
        rebuildingIface->initialize();
        driveIface =
            objServer.add_interface(itemIface->get_object_path(),
                                    "xyz.openbmc_project.Inventory.Item.Drive");
        driveIface->initialize();
    }
    virtual ~Drive()
    {
        objServer.remove_interface(itemIface);
        objServer.remove_interface(operationalIface);
        objServer.remove_interface(rebuildingIface);
        objServer.remove_interface(assetIface);
        objServer.remove_interface(driveIface);
    }

    void createAsset(
        const boost::container::flat_map<std::string, std::string>& data)
    {
        if (assetIface != nullptr)
        {
            return;
        }
        assetIface = objServer.add_interface(
            itemIface->get_object_path(),
            "xyz.openbmc_project.Inventory.Decorator.Asset");
        for (const auto& [key, value] : data)
        {
            assetIface->register_property(key, value);
        }
        assetIface->initialize();
    }

    std::shared_ptr<sdbusplus::asio::dbus_interface> itemIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> operationalIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> rebuildingIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> assetIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> driveIface;

    bool isNvme;
};

struct Backplane
{

    Backplane(size_t busIn, size_t addressIn, size_t backplaneIndexIn,
              const std::string& nameIn) :
        bus(busIn),
        address(addressIn), backplaneIndex(backplaneIndexIn - 1), name(nameIn),
        timer(std::make_shared<boost::asio::steady_timer>(io)),
        muxes(std::make_shared<boost::container::flat_set<Mux>>())
    {
    }
    void run()
    {
        file = open(("/dev/i2c-" + std::to_string(bus)).c_str(),
                    O_RDWR | O_CLOEXEC);
        if (file < 0)
        {
            std::cerr << "unable to open bus " << bus << "\n";
            return;
        }

        if (ioctl(file, I2C_SLAVE_FORCE, address) < 0)
        {
            std::cerr << "unable to set address to " << address << "\n";
            return;
        }

        if (!getPresent())
        {
            std::cerr << "Cannot detect CPLD\n";
            return;
        }

        getBootVer(bootVer);
        getFPGAVer(fpgaVer);
        getSecurityRev(securityRev);
        std::string dbusName = boost::replace_all_copy(name, " ", "_");
        hsbpItemIface = objServer.add_interface(
            "/xyz/openbmc_project/inventory/item/hsbp/" + dbusName,
            inventory::interface);
        hsbpItemIface->register_property("Present", true);
        hsbpItemIface->register_property("PrettyName", name);
        hsbpItemIface->initialize();

        versionIface =
            objServer.add_interface(hsbpItemIface->get_object_path(),
                                    "xyz.openbmc_project.Software.Version");
        versionIface->register_property("Version", zeroPad(bootVer) + "." +
                                                       zeroPad(fpgaVer) + "." +
                                                       zeroPad(securityRev));
        versionIface->register_property(
            "Purpose",
            std::string(
                "xyz.openbmc_project.Software.Version.VersionPurpose.HSBP"));
        versionIface->initialize();
        getPresence(presence);
        getIFDET(ifdet);

        createDrives();

        runTimer();
    }

    void runTimer()
    {
        timer->expires_after(std::chrono::seconds(scanRateSeconds));
        timer->async_wait([this](boost::system::error_code ec) {
            if (ec == boost::asio::error::operation_aborted)
            {
                // we're being destroyed
                return;
            }
            else if (ec)
            {
                std::cerr << "timer error " << ec.message() << "\n";
                return;
            }

            if (!isPowerOn())
            {
                // can't access hsbp when power is off
                runTimer();
                return;
            }
            uint8_t curPresence = 0;
            uint8_t curIFDET = 0;
            uint8_t curFailed = 0;
            uint8_t curRebuild = 0;

            getPresence(curPresence);
            getIFDET(curIFDET);
            getFailed(curFailed);
            getRebuild(curRebuild);

            if (curPresence != presence || curIFDET != ifdet ||
                curFailed != failed || curRebuild != rebuilding)
            {
                presence = curPresence;
                ifdet = curIFDET;
                failed = curFailed;
                rebuilding = curRebuild;
                updateDrives();
            }
            runTimer();
        });
    }

    void createDrives()
    {
        uint8_t nvme = ifdet ^ presence;
        for (size_t ii = 0; ii < maxDrives; ii++)
        {
            bool isNvme = nvme & (1 << ii);
            bool isPresent = isNvme || (presence & (1 << ii));
            bool isFailed = !isPresent || failed & (1 << ii);
            bool isRebuilding = !isPresent && (rebuilding & (1 << ii));

            // +1 to convert from 0 based to 1 based
            size_t driveIndex = (backplaneIndex * maxDrives) + ii + 1;
            Drive& drive = drives.emplace_back(driveIndex, isPresent, !isFailed,
                                               isNvme, isRebuilding);
            std::shared_ptr<Led> led = leds.emplace_back(std::make_shared<Led>(
                drive.itemIface->get_object_path(), ii, file));
            led->createInterface();
        }
    }

    void updateDrives()
    {

        uint8_t nvme = ifdet ^ presence;
        for (size_t ii = 0; ii < maxDrives; ii++)
        {
            bool isNvme = nvme & (1 << ii);
            bool isPresent = isNvme || (presence & (1 << ii));
            bool isFailed = !isPresent || (failed & (1 << ii));
            bool isRebuilding = isPresent && (rebuilding & (1 << ii));

            Drive& drive = drives[ii];
            drive.isNvme = isNvme;
            drive.itemIface->set_property("Present", isPresent);
            drive.operationalIface->set_property("Functional", !isFailed);
            drive.rebuildingIface->set_property("Rebuilding", isRebuilding);
        }
    }

    bool getPresent()
    {
        present = i2c_smbus_read_byte(file) >= 0;
        return present;
    }

    bool getTypeID(uint8_t& val)
    {
        constexpr uint8_t addr = 2;
        int ret = i2c_smbus_read_byte_data(file, addr);
        if (ret < 0)
        {
            std::cerr << "Error " << __FUNCTION__ << "\n";
            return false;
        }
        val = static_cast<uint8_t>(ret);
        return true;
    }

    bool getBootVer(uint8_t& val)
    {
        constexpr uint8_t addr = 3;
        int ret = i2c_smbus_read_byte_data(file, addr);
        if (ret < 0)
        {
            std::cerr << "Error " << __FUNCTION__ << "\n";
            return false;
        }
        val = static_cast<uint8_t>(ret);
        return true;
    }

    bool getFPGAVer(uint8_t& val)
    {
        constexpr uint8_t addr = 4;
        int ret = i2c_smbus_read_byte_data(file, addr);
        if (ret < 0)
        {
            std::cerr << "Error " << __FUNCTION__ << "\n";
            return false;
        }
        val = static_cast<uint8_t>(ret);
        return true;
    }

    bool getSecurityRev(uint8_t& val)
    {
        constexpr uint8_t addr = 5;
        int ret = i2c_smbus_read_byte_data(file, addr);
        if (ret < 0)
        {
            std::cerr << "Error " << __FUNCTION__ << "\n";
            return false;
        }
        val = static_cast<uint8_t>(ret);
        return true;
    }

    bool getPresence(uint8_t& val)
    {
        // NVMe drives do not assert PRSNTn, and as such do not get reported as
        // PRESENT in this register

        constexpr uint8_t addr = 8;

        int ret = i2c_smbus_read_byte_data(file, addr);
        if (ret < 0)
        {
            std::cerr << "Error " << __FUNCTION__ << "\n";
            return false;
        }
        // presence is inverted
        val = static_cast<uint8_t>(~ret);
        return true;
    }

    bool getIFDET(uint8_t& val)
    {
        // This register is a bitmap of parallel GPIO pins connected to the
        // IFDETn pin of a drive slot. SATA, SAS, and NVMe drives all assert
        // IFDETn low when they are inserted into the HSBP.This register, in
        // combination with the PRESENCE register, are used by the BMC to detect
        // the presence of NVMe drives.

        constexpr uint8_t addr = 9;

        int ret = i2c_smbus_read_byte_data(file, addr);
        if (ret < 0)
        {
            std::cerr << "Error " << __FUNCTION__ << "\n";
            return false;
        }
        // ifdet is inverted
        val = static_cast<uint8_t>(~ret);
        return true;
    }

    bool getFailed(uint8_t& val)
    {
        constexpr uint8_t addr = 0xC;
        int ret = i2c_smbus_read_byte_data(file, addr);
        if (ret < 0)
        {
            std::cerr << "Error " << __FUNCTION__ << "\n";
            return false;
        }
        val = static_cast<uint8_t>(ret);
        return true;
    }

    bool getRebuild(uint8_t& val)
    {
        constexpr uint8_t addr = 0xD;
        int ret = i2c_smbus_read_byte_data(file, addr);
        if (ret < 0)
        {
            std::cerr << "Error " << __FUNCTION__ << "\n";
            return false;
        }
        val = static_cast<uint8_t>(ret);
        return true;
    }

    virtual ~Backplane()
    {
        objServer.remove_interface(hsbpItemIface);
        objServer.remove_interface(versionIface);
        if (file >= 0)
        {
            close(file);
        }
    }

    size_t bus;
    size_t address;
    size_t backplaneIndex;
    std::string name;
    std::shared_ptr<boost::asio::steady_timer> timer;
    bool present = false;
    uint8_t typeId = 0;
    uint8_t bootVer = 0;
    uint8_t fpgaVer = 0;
    uint8_t securityRev = 0;
    uint8_t funSupported = 0;
    uint8_t presence = 0;
    uint8_t ifdet = 0;
    uint8_t failed = 0;
    uint8_t rebuilding = 0;

    int file = -1;

    std::string type;

    std::shared_ptr<sdbusplus::asio::dbus_interface> hsbpItemIface;
    std::shared_ptr<sdbusplus::asio::dbus_interface> versionIface;

    std::vector<Drive> drives;
    std::vector<std::shared_ptr<Led>> leds;
    std::shared_ptr<boost::container::flat_set<Mux>> muxes;
};

std::unordered_map<std::string, Backplane> backplanes;
std::vector<Drive> ownerlessDrives; // drives without a backplane

static size_t getDriveCount()
{
    size_t count = 0;
    for (const auto& [key, backplane] : backplanes)
    {
        count += backplane.drives.size();
    }
    return count + ownerlessDrives.size();
}

void updateAssets()
{
    static constexpr const char* nvmeType =
        "xyz.openbmc_project.Inventory.Item.NVMe";
    static const std::string assetTag =
        "xyz.openbmc_project.Inventory.Decorator.Asset";

    conn->async_method_call(
        [](const boost::system::error_code ec, const GetSubTreeType& subtree) {
            if (ec)
            {
                std::cerr << "Error contacting mapper " << ec.message() << "\n";
                return;
            }

            // drives may get an owner during this, or we might disover more
            // drives
            ownerlessDrives.clear();
            for (const auto& [path, objDict] : subtree)
            {
                if (objDict.empty())
                {
                    continue;
                }

                const std::string& owner = objDict.begin()->first;
                // we export this interface too
                if (owner == busName)
                {
                    continue;
                }
                if (std::find(objDict.begin()->second.begin(),
                              objDict.begin()->second.end(),
                              assetTag) == objDict.begin()->second.end())
                {
                    // no asset tag to associate to
                    continue;
                }

                conn->async_method_call(
                    [path](const boost::system::error_code ec2,
                           const boost::container::flat_map<
                               std::string,
                               std::variant<uint64_t, std::string>>& values) {
                        if (ec2)
                        {
                            std::cerr << "Error Getting Config "
                                      << ec2.message() << " " << __FUNCTION__
                                      << "\n";
                            return;
                        }
                        auto findBus = values.find("Bus");

                        if (findBus == values.end())
                        {
                            std::cerr << "Illegal interface at " << path
                                      << "\n";
                            return;
                        }

                        // find the mux bus and addr
                        size_t muxBus = static_cast<size_t>(
                            std::get<uint64_t>(findBus->second));
                        std::filesystem::path muxPath =
                            "/sys/bus/i2c/devices/i2c-" +
                            std::to_string(muxBus) + "/mux_device";
                        if (!std::filesystem::is_symlink(muxPath))
                        {
                            std::cerr << path << " mux does not exist\n";
                            return;
                        }

                        // we should be getting something of the form 7-0052
                        // for bus 7 addr 52
                        std::string fname =
                            std::filesystem::read_symlink(muxPath).filename();
                        auto findDash = fname.find('-');

                        if (findDash == std::string::npos ||
                            findDash + 1 >= fname.size())
                        {
                            std::cerr << path << " mux path invalid\n";
                            return;
                        }

                        std::string busStr = fname.substr(0, findDash);
                        std::string muxStr = fname.substr(findDash + 1);

                        size_t bus = static_cast<size_t>(std::stoi(busStr));
                        size_t addr =
                            static_cast<size_t>(std::stoi(muxStr, nullptr, 16));
                        size_t muxIndex = 0;

                        // find the channel of the mux the drive is on
                        std::ifstream nameFile("/sys/bus/i2c/devices/i2c-" +
                                               std::to_string(muxBus) +
                                               "/name");
                        if (!nameFile)
                        {
                            std::cerr << "Unable to open name file of bus "
                                      << muxBus << "\n";
                            return;
                        }

                        std::string nameStr;
                        std::getline(nameFile, nameStr);

                        // file is of the form "i2c-4-mux (chan_id 1)", get chan
                        // assume single digit chan
                        const std::string prefix = "chan_id ";
                        size_t findId = nameStr.find(prefix);
                        if (findId == std::string::npos ||
                            findId + 1 >= nameStr.size())
                        {
                            std::cerr << "Illegal name file on bus " << muxBus
                                      << "\n";
                        }

                        std::string indexStr =
                            nameStr.substr(findId + prefix.size(), 1);

                        size_t driveIndex = std::stoi(indexStr);

                        Backplane* parent = nullptr;
                        for (auto& [name, backplane] : backplanes)
                        {
                            muxIndex = 0;
                            for (const Mux& mux : *(backplane.muxes))
                            {
                                if (bus == mux.bus && addr == mux.address)
                                {
                                    parent = &backplane;
                                    break;
                                }
                                muxIndex += mux.channels;
                            }
                        }
                        boost::container::flat_map<std::string, std::string>
                            assetInventory;
                        const std::array<const char*, 4> assetKeys = {
                            "PartNumber", "SerialNumber", "Manufacturer",
                            "Model"};
                        for (const auto& [key, value] : values)
                        {
                            if (std::find(assetKeys.begin(), assetKeys.end(),
                                          key) == assetKeys.end())
                            {
                                continue;
                            }
                            assetInventory[key] = std::get<std::string>(value);
                        }

                        // assume its a M.2 or something without a hsbp
                        if (parent == nullptr)
                        {
                            auto& drive = ownerlessDrives.emplace_back(
                                getDriveCount() + 1, true, true, true, false);
                            drive.createAsset(assetInventory);
                            return;
                        }

                        driveIndex += muxIndex;

                        if (parent->drives.size() <= driveIndex)
                        {
                            std::cerr << "Illegal drive index at " << path
                                      << " " << driveIndex << "\n";
                            return;
                        }

                        Drive& drive = parent->drives[driveIndex];
                        drive.createAsset(assetInventory);
                    },
                    owner, path, "org.freedesktop.DBus.Properties", "GetAll",
                    "" /*all interface items*/);
            }
        },
        mapper::busName, mapper::path, mapper::interface, mapper::subtree, "/",
        0, std::array<const char*, 1>{nvmeType});
}

void populateMuxes(std::shared_ptr<boost::container::flat_set<Mux>> muxes,
                   std::string& rootPath)
{
    const static std::array<const std::string, 4> muxTypes = {
        "xyz.openbmc_project.Configuration.PCA9543Mux",
        "xyz.openbmc_project.Configuration.PCA9544Mux",
        "xyz.openbmc_project.Configuration.PCA9545Mux",
        "xyz.openbmc_project.Configuration.PCA9546Mux"};
    conn->async_method_call(
        [muxes](const boost::system::error_code ec,
                const GetSubTreeType& subtree) {
            if (ec)
            {
                std::cerr << "Error contacting mapper " << ec.message() << "\n";
                return;
            }
            std::shared_ptr<std::function<void()>> callback =
                std::make_shared<std::function<void()>>(
                    []() { updateAssets(); });
            size_t index = 0; // as we use a flat map, these are sorted
            for (const auto& [path, objDict] : subtree)
            {
                if (objDict.empty() || objDict.begin()->second.empty())
                {
                    continue;
                }

                const std::string& owner = objDict.begin()->first;
                const std::vector<std::string>& interfaces =
                    objDict.begin()->second;

                const std::string* interface = nullptr;
                for (const std::string& iface : interfaces)
                {
                    if (std::find(muxTypes.begin(), muxTypes.end(), iface) !=
                        muxTypes.end())
                    {
                        interface = &iface;
                        break;
                    }
                }
                if (interface == nullptr)
                {
                    std::cerr << "Cannot get mux type\n";
                    continue;
                }

                conn->async_method_call(
                    [path, muxes, callback, index](
                        const boost::system::error_code ec2,
                        const boost::container::flat_map<
                            std::string,
                            std::variant<uint64_t, std::vector<std::string>>>&
                            values) {
                        if (ec2)
                        {
                            std::cerr << "Error Getting Config "
                                      << ec2.message() << " " << __FUNCTION__
                                      << "\n";
                            return;
                        }
                        auto findBus = values.find("Bus");
                        auto findAddress = values.find("Address");
                        auto findChannelNames = values.find("ChannelNames");
                        if (findBus == values.end() ||
                            findAddress == values.end())
                        {
                            std::cerr << "Illegal configuration at " << path
                                      << "\n";
                            return;
                        }
                        size_t bus = static_cast<size_t>(
                            std::get<uint64_t>(findBus->second));
                        size_t address = static_cast<size_t>(
                            std::get<uint64_t>(findAddress->second));
                        std::vector<std::string> channels =
                            std::get<std::vector<std::string>>(
                                findChannelNames->second);
                        muxes->emplace(bus, address, channels.size(), index);
                        if (callback.use_count() == 1)
                        {
                            (*callback)();
                        }
                    },
                    owner, path, "org.freedesktop.DBus.Properties", "GetAll",
                    *interface);
                index++;
            }
        },
        mapper::busName, mapper::path, mapper::interface, mapper::subtree,
        rootPath, 1, muxTypes);
}

void populate()
{
    conn->async_method_call(
        [](const boost::system::error_code ec, const GetSubTreeType& subtree) {
            if (ec)
            {
                std::cerr << "Error contacting mapper " << ec.message() << "\n";
                return;
            }
            for (const auto& [path, objDict] : subtree)
            {
                if (objDict.empty())
                {
                    continue;
                }

                const std::string& owner = objDict.begin()->first;
                conn->async_method_call(
                    [path](const boost::system::error_code ec2,
                           const boost::container::flat_map<
                               std::string, BasicVariantType>& resp) {
                        if (ec2)
                        {
                            std::cerr << "Error Getting Config "
                                      << ec2.message() << "\n";
                            return;
                        }
                        backplanes.clear();
                        std::optional<size_t> bus;
                        std::optional<size_t> address;
                        std::optional<size_t> backplaneIndex;
                        std::optional<std::string> name;
                        for (const auto& [key, value] : resp)
                        {
                            if (key == "Bus")
                            {
                                bus = std::get<uint64_t>(value);
                            }
                            else if (key == "Address")
                            {
                                address = std::get<uint64_t>(value);
                            }
                            else if (key == "Index")
                            {
                                backplaneIndex = std::get<uint64_t>(value);
                            }
                            else if (key == "Name")
                            {
                                name = std::get<std::string>(value);
                            }
                        }
                        if (!bus || !address || !name || !backplaneIndex)
                        {
                            std::cerr << "Illegal configuration at " << path
                                      << "\n";
                            return;
                        }
                        std::string parentPath =
                            std::filesystem::path(path).parent_path();
                        const auto& [backplane, status] = backplanes.emplace(
                            *name,
                            Backplane(*bus, *address, *backplaneIndex, *name));
                        backplane->second.run();
                        populateMuxes(backplane->second.muxes, parentPath);
                    },
                    owner, path, "org.freedesktop.DBus.Properties", "GetAll",
                    configType);
            }
        },
        mapper::busName, mapper::path, mapper::interface, mapper::subtree, "/",
        0, std::array<const char*, 1>{configType});
}

int main()
{
    boost::asio::steady_timer callbackTimer(io);

    conn->request_name(busName);

    sdbusplus::bus::match::match match(
        *conn,
        "type='signal',member='PropertiesChanged',arg0='" +
            std::string(configType) + "'",
        [&callbackTimer](sdbusplus::message::message&) {
            callbackTimer.expires_after(std::chrono::seconds(2));
            callbackTimer.async_wait([](const boost::system::error_code ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    // timer was restarted
                    return;
                }
                else if (ec)
                {
                    std::cerr << "Timer error" << ec.message() << "\n";
                    return;
                }
                populate();
            });
        });

    sdbusplus::bus::match::match drive(
        *conn,
        "type='signal',member='PropertiesChanged',arg0='xyz.openbmc_project."
        "Inventory.Item.NVMe'",
        [&callbackTimer](sdbusplus::message::message& message) {
            callbackTimer.expires_after(std::chrono::seconds(2));
            if (message.get_sender() == conn->get_unique_name())
            {
                return;
            }
            callbackTimer.async_wait([](const boost::system::error_code ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    // timer was restarted
                    return;
                }
                else if (ec)
                {
                    std::cerr << "Timer error" << ec.message() << "\n";
                    return;
                }
                populate();
            });
        });

    io.post([]() { populate(); });
    setupPowerMatch(conn);
    io.run();
}
