#############################################################################
# Edgecore
#
# Module contains an implementation of SONiC Platform Base API and
# provides the fan status which are available in the platform
# Base PCIe class
#############################################################################

try:
    from sonic_platform_base.sonic_pcie.pcie_common import PcieUtil
except ImportError as e:
    raise ImportError(str(e) + "- required module not found")


class Pcie(PcieUtil):
    """Edgecore Platform-specific PCIe class"""

    def __init__(self, platform_path):
        PcieUtil.__init__(self, platform_path)
        self._conf_rev = self.__get_conf_rev()

    def __get_conf_rev(self):
        """
        Gets the system EEPROM label revision and selects the matching pcie.yaml file.

        This function loads the PDDF API and initializes an Eeprom object. It retrieves
        the TLV field for the label revision from the system EEPROM data. If the field
        is found and valid, it decodes the label from ASCII and returns it.

        Returns:
            str: The decoded label revision if found and valid.
            None: If the labe revision is not found or any error occurs.
        """
        try:
            import os
            from sonic_platform_pddf_base import pddfapi
            from sonic_platform.eeprom import Eeprom

            pddf_obj = pddfapi.PddfApi()
            eeprom = Eeprom(pddf_obj, "{}") # The content of pd-plugin.json is not needed.
            if eeprom is not None:
                 # Try to get the TLV field for the label revision
                (is_valid, results) = eeprom.get_tlv_field(eeprom.eeprom_data, eeprom._TLV_CODE_LABEL_REVISION)
                if not is_valid or results[2] is None:
                    return None

                label_rev = results[2].decode('ascii')

                for rev in (label_rev, label_rev[:-1]):
                    pcie_yaml_file = os.path.join(self.config_path, f"pcie_{rev}.yaml")
                    if os.path.exists(pcie_yaml_file):
                        return rev

        except Exception as e:
            pass

        return None
