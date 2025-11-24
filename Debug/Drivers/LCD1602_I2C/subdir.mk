################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (11.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Drivers/LCD1602_I2C/lcd1602_i2c.c 

OBJS += \
./Drivers/LCD1602_I2C/lcd1602_i2c.o 

C_DEPS += \
./Drivers/LCD1602_I2C/lcd1602_i2c.d 


# Each subdirectory must supply rules for building sources it contributes
Drivers/LCD1602_I2C/%.o Drivers/LCD1602_I2C/%.su Drivers/LCD1602_I2C/%.cyclo: ../Drivers/LCD1602_I2C/%.c Drivers/LCD1602_I2C/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F407xx -c -I../Core/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I"D:/STM32CubeIDE/workspace_1.14.1/Project0/FreeRTOS/include" -I"D:/STM32CubeIDE/workspace_1.14.1/Project0/FreeRTOS/portable/ARM_CM4F" -I"D:/STM32CubeIDE/workspace_1.14.1/Project0/Drivers/LCD1602_I2C" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Drivers-2f-LCD1602_I2C

clean-Drivers-2f-LCD1602_I2C:
	-$(RM) ./Drivers/LCD1602_I2C/lcd1602_i2c.cyclo ./Drivers/LCD1602_I2C/lcd1602_i2c.d ./Drivers/LCD1602_I2C/lcd1602_i2c.o ./Drivers/LCD1602_I2C/lcd1602_i2c.su

.PHONY: clean-Drivers-2f-LCD1602_I2C

