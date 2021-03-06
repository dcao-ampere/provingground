/*
// Copyright (c) 2018 Intel Corporation
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

#include "specialmodemgr.hpp"
#include "file.hpp"

#include <sys/sysinfo.h>

#include <pwd.h>
#include <shadow.h>
#include <fstream>
#include <phosphor-logging/log.hpp>
#include <string>

namespace specialMode
{
static constexpr const char* specialModeMgrService =
    "xyz.openbmc_project.SpecialMode";
static constexpr const char* specialModeIntf =
    "xyz.openbmc_project.Security.SpecialMode";
static constexpr const char* specialModePath =
    "/xyz/openbmc_project/security/special_mode";
static constexpr const char* provisioningMode =
    "xyz.openbmc_project.Control.Security.RestrictionMode.Modes.Provisioning";
static constexpr const char* restrictionModeService =
    "xyz.openbmc_project.RestrictionMode.Manager";
static constexpr const char* restrictionModeIntf =
    "xyz.openbmc_project.Control.Security.RestrictionMode";

static constexpr const char* restrictionModeProperty = "RestrictionMode";
static constexpr int mtmAllowedTime = 15 * 60; // 15 minutes

using VariantValue =
    std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                 uint64_t, double, std::string>;

namespace secCtrl = sdbusplus::xyz::openbmc_project::Control::Security::server;

#ifdef BMC_VALIDATION_UNSECURE_FEATURE

static void checkAndConfigureSpecialUser()
{
    std::array<char, 4096> sbuffer{};
    struct spwd spwd;
    struct spwd* resultPtr = nullptr;
    constexpr const char* specialUser = "root";

    // Query shadow entry for special user.
    int status = getspnam_r(specialUser, &spwd, sbuffer.data(),
                            sbuffer.max_size(), &resultPtr);
    if (status || (&spwd != resultPtr))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Error in querying shadow entry for special user");
    }
    // Password will be single character '!' or '*' for disabled login
    if ((resultPtr->sp_pwdp[0] == '!' || resultPtr->sp_pwdp[0] == '*') &&
        resultPtr->sp_pwdp[1] == 0)
    {
        File passwdFd("/etc/shadow", "r+");
        if ((passwdFd)() == nullptr)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Error in opening shadow file");
            return;
        }
        // Mark the special user password as null, to allow
        // nullok login.
        resultPtr->sp_pwdp[0] = 0;
        // Mark the special user password as expired. This will
        // force user to update new password on first login.
        resultPtr->sp_lstchg = 0;
        putspent(resultPtr, (passwdFd)());
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "Configured special user sucessfully");
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::DEBUG>(
            "Skip configuring special user as it is already enabled");
    }
}

#endif

SpecialModeMgr::SpecialModeMgr(
    boost::asio::io_service& io_, sdbusplus::asio::object_server& srv_,
    std::shared_ptr<sdbusplus::asio::connection>& conn_) :
    io(io_),
    server(srv_), conn(conn_),
    timer(std::make_unique<boost::asio::steady_timer>(io)),
    specialMode(secCtrl::SpecialMode::Modes::None)
{

#ifdef BMC_VALIDATION_UNSECURE_FEATURE
    if (std::filesystem::exists(validationModeFile))
    {
        specialMode = secCtrl::SpecialMode::Modes::ValidationUnsecure;
        addSpecialModeProperty();
        return;
    }
#endif

    // Following condition must match to indicate specialMode.
    // Mark the mode as None for any failure.
    // 1. U-Boot detected power button press & indicated "special=mfg"
    // in command line parameter.
    // 2. BMC in Provisioning mode.
    // 3. BMC boot is due to AC cycle.
    // 4. Not crossed 12 hours in this special case.
    std::string cmdLineStr;
    std::ifstream cmdLineIfs("/proc/cmdline");
    getline(cmdLineIfs, cmdLineStr);
    static constexpr const char* specialModeStr = "special=mfg";
    static constexpr const char* acBootStr = "resetreason=0x11";
    if ((cmdLineStr.find(specialModeStr) != std::string::npos) &&
        (cmdLineStr.find(acBootStr) != std::string::npos))
    {
        intfAddMatchRule = std::make_unique<sdbusplus::bus::match::match>(
            static_cast<sdbusplus::bus::bus&>(*conn),
            "type='signal',member='InterfacesAdded',sender='" +
                std::string(restrictionModeService) + "'",
            [this](sdbusplus::message::message& message) {
                sdbusplus::message::object_path objectPath;
                std::map<std::string, std::map<std::string, VariantValue>>
                    objects;
                message.read(objectPath, objects);
                VariantValue mode;
                try
                {
                    std::map<std::string, VariantValue> prop =
                        objects.at(restrictionModeIntf);
                    mode = prop.at(restrictionModeProperty);
                }
                catch (const std::out_of_range& e)
                {
                    phosphor::logging::log<phosphor::logging::level::ERR>(
                        "Error in finding RestrictionMode property");

                    return;
                }
                checkAndAddSpecialModeProperty(std::get<std::string>(mode));
            });

        propUpdMatchRule = std::make_unique<sdbusplus::bus::match::match>(
            static_cast<sdbusplus::bus::bus&>(*conn),
            "type='signal',member='PropertiesChanged', "
            "interface='org.freedesktop.DBus.Properties', "
            "arg0namespace='xyz.openbmc_project.Control.Security."
            "RestrictionMode'",
            [this](sdbusplus::message::message& message) {
                std::string intfName;
                std::map<std::string, std::variant<std::string>> properties;

                // Skip reading 3rd argument (old porperty value)
                message.read(intfName, properties);

                std::variant<std::string> mode;
                try
                {
                    mode = properties.at(restrictionModeProperty);
                }

                catch (const std::out_of_range& e)
                {
                    phosphor::logging::log<phosphor::logging::level::ERR>(
                        "Error in finding RestrictionMode property");

                    return;
                }

                if (std::get<std::string>(mode) != provisioningMode)
                {
                    phosphor::logging::log<phosphor::logging::level::INFO>(
                        "Mode is not provisioning");
                    setSpecialModeValue(secCtrl::SpecialMode::Modes::None);
                }
            });

        conn->async_method_call(
            [this](boost::system::error_code ec, const VariantValue& mode) {
                if (ec)
                {
                    phosphor::logging::log<phosphor::logging::level::INFO>(
                        "Error in reading restrictionMode property, probably "
                        "service not up");
                    return;
                }
                checkAndAddSpecialModeProperty(std::get<std::string>(mode));
            },
            restrictionModeService,
            "/xyz/openbmc_project/control/security/restriction_mode",
            "org.freedesktop.DBus.Properties", "Get", restrictionModeIntf,
            restrictionModeProperty);
    }
    else
    {
        addSpecialModeProperty();
    }
}

void SpecialModeMgr::checkAndAddSpecialModeProperty(const std::string& provMode)
{
    if (iface != nullptr && iface->is_initialized())
    {
        // Already initialized
        return;
    }
    if (provMode != provisioningMode)
    {
        addSpecialModeProperty();
        return;
    }
    struct sysinfo sysInfo = {};
    int ret = sysinfo(&sysInfo);
    if (ret != 0)
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "ERROR in getting sysinfo",
            phosphor::logging::entry("RET = %d", ret));
        addSpecialModeProperty();
        return;
    }
    int specialModeLockoutSeconds = 0;
    if (mtmAllowedTime > sysInfo.uptime)
    {
        specialMode = secCtrl::SpecialMode::Modes::Manufacturing;
        specialModeLockoutSeconds = mtmAllowedTime - sysInfo.uptime;
        sd_journal_send("MESSAGE=%s", "Manufacturing mode - Entered",
                        "PRIORITY=%i", LOG_INFO, "REDFISH_MESSAGE_ID=%s",
                        "OpenBMC.0.1.ManufacturingModeEntered", NULL);
#ifdef BMC_VALIDATION_UNSECURE_FEATURE
        checkAndConfigureSpecialUser();
#endif
    }
    addSpecialModeProperty();
    if (!specialModeLockoutSeconds)
    {
        return;
    }
    updateTimer(specialModeLockoutSeconds);
}

void SpecialModeMgr::addSpecialModeProperty()
{
    // Add path to server object
    iface = server.add_interface(specialModePath, specialModeIntf);
    iface->register_property(
        strSpecialMode, secCtrl::convertForMessage(specialMode),
        // Ignore set
        [this](const std::string& req, std::string& propertyValue) {
            secCtrl::SpecialMode::Modes mode =
                secCtrl::SpecialMode::convertModesFromString(req);
#ifdef BMC_VALIDATION_UNSECURE_FEATURE
            if ((mode == secCtrl::SpecialMode::Modes::ValidationUnsecure) &&
                (specialMode != mode))
            {
                std::ofstream output(validationModeFile);
                output.close();
                specialMode = mode;
                propertyValue = req;
                return 1;
            }
#endif

            if (mode == secCtrl::SpecialMode::Modes::None &&
                specialMode != mode)
            {
#ifdef BMC_VALIDATION_UNSECURE_FEATURE
                std::remove(validationModeFile.c_str());
#endif
                specialMode = mode;
                propertyValue = req;
                return 1;
            }
            return 0;
        },
        // Override get
        [this](const std::string& mode) {
            return secCtrl::convertForMessage(specialMode);
        });
    iface->register_method("ResetTimer", [this]() {
        if (specialMode == secCtrl::SpecialMode::Modes::Manufacturing)
        {
            updateTimer(mtmAllowedTime);
        }
        return;
    });
    iface->initialize(true);
}

void SpecialModeMgr::updateTimer(int countInSeconds)
{
    timer->expires_after(std::chrono::seconds(countInSeconds));
    timer->async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            // timer aborted
            return;
        }
        else if (ec)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Error in special mode timer");
            return;
        }
#ifdef BMC_VALIDATION_UNSECURE_FEATURE
        if (specialMode == secCtrl::SpecialMode::Modes::ValidationUnsecure)
        {
            // Don't reset, if in ValidationUnsecure mode
            return;
        }
#endif
        iface->set_property(
            strSpecialMode,
            secCtrl::convertForMessage(secCtrl::SpecialMode::Modes::None));
        sd_journal_send("MESSAGE=%s", "Manufacturing mode - Exited",
                        "PRIORITY=%i", LOG_INFO, "REDFISH_MESSAGE_ID=%s",
                        "OpenBMC.0.1.ManufacturingModeExited", NULL);
    });
}

} // namespace specialMode

int main()
{
    using namespace specialMode;
    boost::asio::io_service io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);
    conn->request_name(specialModeMgrService);
    sdbusplus::asio::object_server server(conn, true);
    auto mgrIntf = server.add_interface(specialModePath, "");
    mgrIntf->initialize();
    server.add_manager(specialModePath);

    SpecialModeMgr specialModeMgr(io, server, conn);
    io.run();
}
