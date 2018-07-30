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

#include "dimm.hpp"
#include "manager.hpp"

namespace phosphor
{
namespace smbios
{

static constexpr uint16_t maxOldDimmSize = 0x7fff;
void Dimm::memoryInfoUpdate(void)
{
    uint8_t *dataIn = regionS[0].regionData;

    dataIn = smbiosTypePtr(dataIn, memoryDeviceType);
    for (uint8_t index = 0; index < dimmNum; index++)
    {
        dataIn = smbiosNextPtr(dataIn);
        if (dataIn == nullptr)
        {
            return;
        }
        dataIn = smbiosTypePtr(dataIn, memoryDeviceType);
        if (dataIn == nullptr)
        {
            return;
        }
    }

    auto memoryInfo = reinterpret_cast<struct MemoryInfo *>(dataIn);

    memoryDataWidth(memoryInfo->dataWidth);

    if (memoryInfo->size == maxOldDimmSize)
    {
        dimmSizeExt(memoryInfo->extendedSize);
    }
    else
    {
        dimmSize(memoryInfo->size);
    }

    dimmDeviceLocator(memoryInfo->deviceLocator, memoryInfo->length, dataIn);
    dimmType(memoryInfo->memoryType);
    dimmTypeDetail(memoryInfo->typeDetail);
    memorySpeed(memoryInfo->speed);
    dimmManufacturer(memoryInfo->manufacturer, memoryInfo->length, dataIn);
    dimmSerialNum(memoryInfo->serialNum, memoryInfo->length, dataIn);
    dimmPartNum(memoryInfo->partNum, memoryInfo->length, dataIn);
    memoryAttributes(memoryInfo->attributes);
    memoryConfClockSpeed(memoryInfo->confClockSpeed);

    return;
}

uint16_t Dimm::memoryDataWidth(uint16_t value)
{
    return sdbusplus::xyz::openbmc_project::Inventory::Item::server::Dimm::
        memoryDataWidth(value);
}

static constexpr uint16_t baseNewVersionDimmSize = 0x8000;
static constexpr uint16_t dimmSizeUnit = 1024;
void Dimm::dimmSize(uint16_t size)
{
    uint32_t result = size & maxOldDimmSize;
    if (0 == (size & baseNewVersionDimmSize))
    {
        result = result * dimmSizeUnit;
    }
    memorySizeInKB(result);
}

void Dimm::dimmSizeExt(uint32_t size)
{
    size = size * dimmSizeUnit;
    memorySizeInKB(size);
}

uint32_t Dimm::memorySizeInKB(uint32_t value)
{
    return sdbusplus::xyz::openbmc_project::Inventory::Item::server::Dimm::
        memorySizeInKB(value);
}

void Dimm::dimmDeviceLocator(uint8_t positionNum, uint8_t structLen,
                             uint8_t *dataIn)
{
    std::string result;

    result = positionToString(positionNum, structLen, dataIn);

    memoryDeviceLocator(result);
}

std::string Dimm::memoryDeviceLocator(std::string value)
{
    return sdbusplus::xyz::openbmc_project::Inventory::Item::server::Dimm::
        memoryDeviceLocator(value);
}

void Dimm::dimmType(uint8_t type)
{
    std::map<uint8_t, std::string>::const_iterator it =
        dimmTypeTable.find(type);
    if (it == dimmTypeTable.end())
    {
        memoryType("Unknown Memory Type");
    }
    else
    {
        memoryType(it->second);
    }
}

std::string Dimm::memoryType(std::string value)
{
    return sdbusplus::xyz::openbmc_project::Inventory::Item::server::Dimm::
        memoryType(value);
}

void Dimm::dimmTypeDetail(uint16_t detail)
{
    std::string result;
    for (uint8_t index = 0; index < (8 * sizeof(detail)); index++)
    {
        if (detail & 0x01)
        {
            result += detailTable[index];
        }
        detail >>= 1;
    }
    memoryTypeDetail(result);
}

std::string Dimm::memoryTypeDetail(std::string value)
{
    return sdbusplus::xyz::openbmc_project::Inventory::Item::server::Dimm::
        memoryTypeDetail(value);
}

uint16_t Dimm::memorySpeed(uint16_t value)
{
    return sdbusplus::xyz::openbmc_project::Inventory::Item::server::Dimm::
        memorySpeed(value);
}

void Dimm::dimmManufacturer(uint8_t positionNum, uint8_t structLen,
                            uint8_t *dataIn)
{
    std::string result;

    result = positionToString(positionNum, structLen, dataIn);

    memoryManufacturer(result);
}

std::string Dimm::memoryManufacturer(std::string value)
{
    return sdbusplus::xyz::openbmc_project::Inventory::Item::server::Dimm::
        memoryManufacturer(value);
}

void Dimm::dimmSerialNum(uint8_t positionNum, uint8_t structLen,
                         uint8_t *dataIn)
{
    std::string result;

    result = positionToString(positionNum, structLen, dataIn);

    memorySerialNum(result);
}

std::string Dimm::memorySerialNum(std::string value)
{
    return sdbusplus::xyz::openbmc_project::Inventory::Item::server::Dimm::
        memorySerialNum(value);
}

void Dimm::dimmPartNum(uint8_t positionNum, uint8_t structLen, uint8_t *dataIn)
{
    std::string result;

    result = positionToString(positionNum, structLen, dataIn);

    memoryPartNum(result);
}

std::string Dimm::memoryPartNum(std::string value)
{
    return sdbusplus::xyz::openbmc_project::Inventory::Item::server::Dimm::
        memoryPartNum(value);
}

uint8_t Dimm::memoryAttributes(uint8_t value)
{
    return sdbusplus::xyz::openbmc_project::Inventory::Item::server::Dimm::
        memoryAttributes(value);
}

uint16_t Dimm::memoryConfClockSpeed(uint16_t value)
{
    return sdbusplus::xyz::openbmc_project::Inventory::Item::server::Dimm::
        memoryConfClockSpeed(value);
}

} // namespace smbios
} // namespace phosphor