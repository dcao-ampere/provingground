#!/usr/bin/python
from string import Template
import os
import sys
import glob
import subprocess
import StringIO
import shutil
import json

CONFIGURATION_DIR = '/var/configuration/'
TEMPLATE_DIR = '/usr/share/overlay_templates'
PLATFORM = 'aspeed,ast2500'
OUTPUT_DIR = '/tmp/overlays'
DTOVERLAY = os.path.join('/usr', 'bin', 'dtoverlay')
OVERLAY_DIR = '/sys/kernel/config/device-tree/overlays/'


class Entity(object):

    def __init__(self):
        pass

    def create_dts(self):
        with open(os.path.join(TEMPLATE_DIR, '{}.template'.format(self.name))) as template_file:
            output = Template(template_file.read()).substitute(
                self.template_args)

            outfile = '{}@{}.dts'.format(self.name, self.template_args['name'])
            with open(os.path.join(OUTPUT_DIR, outfile), 'w') as file_handle:
                file_handle.write(output)

    def create_dtbo(self):
        dts = os.path.join(
            OUTPUT_DIR, '{}@{}.dts'.format(self.name, self.template_args['name']))
        dtb = dts.replace('.dts', '.dtbo')
        subprocess.check_call(
            r'dtc -@ -I dts -O dtb -o {} {}'.format(dtb, dts), shell=True)

    def load_overlay(self):
        dtb = os.path.join(OUTPUT_DIR, '{}@{}.dtbo'.format(
            self.name, self.template_args['name']))
        basename, _ = os.path.splitext(os.path.basename(dtb))
        subprocess.check_call(r'{} -d {} {}'.format(
            DTOVERLAY, OUTPUT_DIR, basename), shell=True)

    def force_probe(self):
        if not hasattr(self, 'probe_driver'):
            return
        for device in glob.glob(os.path.join(self.probe_driver, '*')):
            if os.path.islink(device):  # devices are symlinks
                with open(os.path.join(self.probe_driver, 'unbind'), 'w') as unbind:
                    unbind.write(os.path.basename(device))
                with open(os.path.join(self.probe_driver, 'bind'), 'w') as bind:
                    bind.write(os.path.basename(device))

    def load_entity(self):
        self.create_dts()
        self.create_dtbo()
        self.load_overlay()


class TMP75(Entity):

    def __init__(self, **kwargs):
        self.name = 'tmp75'
        self.template_args = {'platform': PLATFORM,
                              'bus': kwargs.get('bus'),
                              'reg': kwargs.get('reg'),
                              'name': kwargs.get('oem_name')}


class TMP421(Entity):

    def __init__(self, **kwargs):
        self.name = 'tmp421'
        self.template_args = {'platform': PLATFORM,
                              'bus': kwargs.get('bus'),
                              'reg': kwargs.get('reg'),
                              'name': kwargs.get('oem_name', ''),
                              'name1': kwargs.get('oem_name1', '')}


class EEPROM(Entity):

    def __init__(self, **kwargs):
        self.name = 'eeprom'
        self.template_args = {'platform': PLATFORM,
                              'bus': kwargs.get('bus'),
                              'reg': kwargs.get('reg'),
                              'name': kwargs.get('oem_name', '')}

class PMBUS(Entity):
    def __init__(self, **kwargs):
        self.name = 'pmbus'
        self.template_args = {'platform': PLATFORM,
                              'bus': kwargs.get('bus'),
                              'reg': kwargs.get('reg'),
                              'name': kwargs.get('oem_name', '')}

class SMBUS_MUX(Entity):
    def __init__(self, **kwargs):
        self.name = 'smbus_mux'
        self.template_args = {'platform': PLATFORM,
                              'bus': kwargs.get('bus'),
                              'reg': kwargs.get('reg'),
                              'name': kwargs.get('oem_name', '')}

class ADC(Entity):

    def __init__(self, **kwargs):
        self.name = 'adc'
        self.template_args = {'platform': PLATFORM,
                              'index': int(kwargs.get('index')) + 1,
                              'name': kwargs.get('oem_name', '')}


class ASPEED_PWMTACHO(Entity):

    def __init__(self, **kwargs):
        self.name = 'pwmtach'
        self.probe_driver = r'/sys/bus/platform/drivers/aspeed_pwm_tacho'

        self.template_args = {'platform': PLATFORM,
                              'tachs': ' '.join(['0x{:02x}'.format(ii) for ii in kwargs.get('tachs')]),
                              'reg': '0x{:02x}'.format(kwargs.get('pwm', '')),
                              'index': kwargs.get('pwm', ''),
                              'name': 'Tach{}'.format(kwargs.get('pwm', ''))}  # todo, fill in real name

    def load_entity(self):
        super(self.__class__, self).load_entity()
        self.force_probe()


class ASPEED_PECI_HWMON(Entity):

    def __init__(self, **kwargs):
        self.name = 'peci_hwmon'
        self.probe_driver = r'/sys/bus/platform/drivers/aspeed-peci-hwmon'

        self.template_args = {'platform': PLATFORM,
                              'name': kwargs.get('oem_name', ''),
                              'cpu_id': int(kwargs.get('cpu_id')),
                              'show_core': int(kwargs.get('show_core')),
                              'dimm_nums': int(kwargs.get('dimm_nums'))}


def get_obj(**kwargs):
    type = kwargs.get('type', None)
    if type is None:
        raise KeyError('Missing type argument.')
    return getattr(sys.modules[__name__], type.upper())(**kwargs)


def unload_overlays():
    active = subprocess.check_output(
        r'{} -d {} -l'.format(DTOVERLAY, OVERLAY_DIR), shell=True)
    buf = StringIO.StringIO(active)
    buf.readline()  # first line isn't important
    for _ in buf:
        subprocess.call(
            r'{} -d {} -r 0'.format(DTOVERLAY, OUTPUT_DIR), shell=True)
    if os.path.exists(OUTPUT_DIR):
        shutil.rmtree(OUTPUT_DIR)


def load_entity(**kwargs):
    if not os.path.exists(OUTPUT_DIR):
        os.makedirs(OUTPUT_DIR)
    obj = get_obj(**kwargs)
    obj.load_entity()


if __name__ == '__main__':
    if '-d' in sys.argv:
        unload_overlays()
    else:
        unload_overlays()
        configurations = json.load(
            open(os.path.join(CONFIGURATION_DIR, 'system.json')))
        for _, configuration in configurations.iteritems():
            for element in configuration.get('exposes', []):
                if not isinstance(
                        element,
                        dict) or element.get(
                    'status',
                            'okay') != 'okay':
                    continue
                element['oem_name'] = element.get(
                    'name', 'unknown').replace(
                    ' ', '_')

                bus = element.get('bus', None)
                if bus and not isinstance(bus, int):
                    element['bus'] = int(bus, 16)

                # todo, this should really just be a map or deleted
                if element.get("type", "") == "TMP75":
                    element["reg"] = element.get("address").lower()
                    load_entity(**element)

                elif element.get("type", "") == "TMP421":
                    element["reg"] = element.get("address").lower()
                    element["oem_name1"] = element.get(
                        "name1").replace(" ", "_")
                    load_entity(**element)

                elif element.get("type", "") == "ADC":
                    load_entity(**element)

                elif element.get("type", "") == "AspeedFan":
                    connector = element['connector']
                    element.update(connector)
                    element["type"] = 'aspeed_pwmtacho'
                    load_entity(**element)

                elif element.get("type", "") == "SkylakeCPU":
                    element["type"] = 'aspeed_peci_hwmon'
                    load_entity(**element)

                elif element.get("type", "") == "IntelFruDevice":
                    element["type"] = 'eeprom'
                    element["reg"] = element.get("address").lower()
                    load_entity(**element)

                elif element.get("type", "") == "pmbus":
                    element["reg"] = element.get("address").lower()
                    load_entity(**element)

                elif element.get("type", "") == "SMBusMux":
                    element["type"] = 'smbus_mux'
                    element["reg"] = element.get("address").lower()
                    load_entity(**element)
