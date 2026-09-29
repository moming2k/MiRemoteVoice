/* Periodic battery measurement feeding the Battery Service. */
#ifndef BATTERY_H_
#define BATTERY_H_

int battery_init(void);
/* Take a measurement now (e.g. right after a connection). */
void battery_measure(void);

#endif /* BATTERY_H_ */
