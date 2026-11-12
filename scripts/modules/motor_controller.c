/* motor_controller.c — fonte da biblioteca do modulo `motor_controller.mod`.
 *
 * Compilado pelo Makefile (target `test`):
 *   cc -shared -fPIC -o scripts/modules/lib/<plat>/<arch>/libmotor_controller.so
 *
 * O manifesto .mod declara quais destes simbolos viram funcoes/variaveis
 * do interpretador (FFI sem libffi; tipos int32/float32 no manifesto).
 */
#include <stdint.h>

int32_t g_MotorSpeedLimit = 3000; /* variavel nativa: MotorSpeedLimit */
float   g_RPM = 0.0f;             /* variavel nativa: RPM */
char    g_MotorName[32] = "MX-200"; /* variavel nativa string: MotorName */

static int32_t g_running = 0;

int32_t Motor_Init(void) {
    g_running = 0;
    return 0; /* != 0 aborta o import (initialize_function) */
}

int32_t Motor_Shutdown(void) {
    g_running = 0;
    g_RPM = 0.0f;
    return 0;
}

int32_t Motor_Start(int32_t rpm) {
    if (rpm <= 0 || rpm > g_MotorSpeedLimit) return -1;
    g_running = 1;
    g_RPM = (float)rpm;
    return 0;
}

int32_t Motor_Stop(void) {
    g_running = 0;
    g_RPM = 0.0f;
    return 0;
}

/* out-params: o interpretador passa ponteiros para temporarios e monta
 * o resultado como array [retorno, rpm_out, temp_out] */
int32_t Motor_Read(int32_t* rpm_out, float* temp_out) {
    if (rpm_out) *rpm_out = g_running ? (int32_t)g_RPM : 0;
    if (temp_out)
        *temp_out = g_running ? 42.5f + (float)g_RPM / 100.0f : 0.0f;
    return g_running ? 0 : -1;
}

int32_t Motor_GetLimit(void) {
    return g_MotorSpeedLimit;
}
