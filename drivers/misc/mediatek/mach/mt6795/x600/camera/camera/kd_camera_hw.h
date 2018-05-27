#ifndef _KD_CAMERA_HW_H_
#define _KD_CAMERA_HW_H_


#include <mach/mt_gpio.h>
#include <mach/mt_pm_ldo.h>
#include "pmic_drv.h"


//rear camera
//Analog 
#define CAMERA_POWER_VCAM_A PMIC_APP_MAIN_CAMERA_POWER_A
//Digital 
#define CAMERA_POWER_VCAM_D PMIC_APP_MAIN_CAMERA_POWER_D
//3G
#define CAMERA_POWER_VCAM_3G PMIC_APP_MAIN_CAMERA_POWER_AF
//AF
#define CAMERA_POWER_VCAM_AF MT6331_POWER_LDO_VMCH
//IO
#define CAMERA_POWER_VCAM_IO PMIC_APP_MAIN_CAMERA_POWER_IO

//The AVDD,IOVDD of front camera is the same as rear camera
//Digital2
#define CAMERA_POWER_VCAM_D2 MT6331_POWER_LDO_VGP3
#define CAMERA_POWER_VCAM_A2 PMIC_APP_MAIN_CAMERA_POWER_A

//FIXME, should defined in DCT tool
//Main sensor
#define CAMERA_CMRST_PIN            GPIO_CAMERA_CMRST_PIN
#define CAMERA_CMRST_PIN_M_GPIO     GPIO_CAMERA_CMRST_PIN_M_GPIO

#define CAMERA_CMPDN_PIN            GPIO_CAMERA_CMRST_PIN
#define CAMERA_CMPDN_PIN_M_GPIO     GPIO_CAMERA_CMRST_PIN_M_GPIO

//FRONT sensor
#define CAMERA_CMRST1_PIN           GPIO_CAMERA_CMRST1_PIN
#define CAMERA_CMRST1_PIN_M_GPIO    GPIO_CAMERA_CMRST1_PIN_M_GPIO

//#define CAMERA_CMPDN1_PIN           GPIO_CAMERA_CMPDN1_PIN 
//#define CAMERA_CMPDN1_PIN_M_GPIO    GPIO_CAMERA_CMPDN1_PIN_M_GPIO
#define CAMERA_CMPDN1_PIN           GPIO_CAMERA_CMRST_PIN 
#define CAMERA_CMPDN1_PIN_M_GPIO    GPIO_CAMERA_CMRST_PIN_M_GPIO

//Main2 sensor
//#define CAMERA_CMRST2_PIN           GPIO_CAMERA_2_CMRST_PIN
//#define CAMERA_CMRST2_PIN_M_GPIO    GPIO_CAMERA_2_CMRST_PIN_M_GPIO

//#define CAMERA_CMPDN2_PIN           GPIO_CAMERA_2_CMPDN_PIN
//#define CAMERA_CMPDN2_PIN_M_GPIO    GPIO_CAMERA_2_CMPDN_PIN_M_GPIO

// Define I2C Bus Num
#define SUPPORT_I2C_BUS_NUM1        0
#define SUPPORT_I2C_BUS_NUM2        2
#define MAIN_CAM_USE_I2C_NUM    SUPPORT_I2C_BUS_NUM1
#define SUB_CAM_USE_I2C_NUM     SUPPORT_I2C_BUS_NUM1
#define MAIN2_CAM_USE_I2C_NUM   SUPPORT_I2C_BUS_NUM2

#endif
