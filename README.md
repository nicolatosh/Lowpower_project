### 1. Low power project

Project of the Low Power wireless radio sensor networks course for IoT a.a 2021/2022

***Abstract:***
The project goal is to develop a protocol for WSN capable to react to sensor *events* and perform the *actuation* actions when the system is *unstable*.
The simulation scenario considers a water tank for which WSN have been deployed. There is a *controller* in charge of receiving sensors updated readings and, in case a tunable percentage of nodes reports the same water lever, the controller can proceed to elaborate the solution strategy. It ends up sending some commands to reset/restore sensors "vision" of the world.

---

##### Platform & tools

- Contiki Os
- TMoteSKY
- Cooja simulator
- Real node demployment scenario ***Iot Testbed @UniTn***

---

##### How the protocol works

Below there are the main steps protocol consist of:

1. Mac layer communication tree constuction
2. Event dissemination: broadcast
3. Data collection of sensed data
4. Command delivery `controller -> sensor/actuator` using MAC tree reverse paths

##### Benckmark and evalutation

Evaluation based on reliability

- PDR, Node duty-cycle, failure management

---

##### Aadditional info

- my_testbed_conf: contains the `.json` file for the experiment
- final_tests_cooja: files related to the final version of the project cooja tests
- testbed_experiments: same as above but related to testbed

