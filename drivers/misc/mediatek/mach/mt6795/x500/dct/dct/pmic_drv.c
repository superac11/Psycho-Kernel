/*
 * Generated by MTK SP Drv_CodeGen Version 03.13.6  for MT6795. Copyright MediaTek Inc. (C) 2013.
 * Sun May 27 17:37:05 2018
 * Do Not Modify the File.
 */



#include <linux/types.h>
#include <mach/mt_typedefs.h>
#include <mach/mt_pm_ldo.h>


void pmu_drv_tool_customization_init(void)
{
    pmic_ldo_enable(MT6331_POWER_LDO_VMCH,KAL_FALSE);


    pmic_ldo_enable(MT6331_POWER_LDO_VIO28,KAL_TRUE);


    pmic_ldo_enable(MT6331_POWER_LDO_VCAM_IO,KAL_TRUE);


}




