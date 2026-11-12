{
  "name": "motor_controller",
  "version": "1.0",
  "platform": "linux",
  "arch": "x86_64",
  "library": "lib/linux/x86_64/libmotor_controller.so",
  "init": "Motor_Init",
  "shutdown": "Motor_Shutdown",
  "functions": [
    {
      "name": "Motor_Start",
      "return": "int32",
      "parameters": [ { "name": "rpm", "type": "int32" } ]
    },
    {
      "name": "Motor_Stop",
      "return": "int32",
      "parameters": []
    },
    {
      "name": "Motor_Read",
      "return": "int32",
      "parameters": [
        { "name": "rpm_out", "type": "int32", "direction": "out" },
        { "name": "temp_out", "type": "float32", "direction": "out" }
      ]
    },
    {
      "name": "Motor_GetLimit",
      "return": "int32",
      "parameters": []
    }
  ],
  "variables": [
    { "name": "MotorSpeedLimit", "symbol": "g_MotorSpeedLimit", "type": "int32" },
    { "name": "RPM", "symbol": "g_RPM", "type": "float32" },
    { "name": "MotorName", "symbol": "g_MotorName", "type": "string",
      "readonly": true, "size": 32 }
  ]
}
