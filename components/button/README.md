# Button Component

[![Badge](https://components.espressif.com/components/espp/button/badge.svg)](https://components.espressif.com/components/espp/button)

The `Button` class provides a simple way to read the state of a button. There
are two ways to configure the functionality of the button.

The first uses `Button::Config` to configure the button to call a function when
the button is pressed or released and uses the `Interrupt` class' task and ISR
for signaling. At any time, a user may also call the `is_pressed` method to read
the current state of the button's input pin.

The second uses `Button::SimpleConfig` to simply configure the button as an
input with configurable pull-up or pull-down resistors and a configurable active
state (high or low). In this configuration, the input is read manually any time
the user calls the `is_pressed` method.

# Example

The [example](./example) shows how to use the `espp::Button` class to handle
input events from a physical button connected to an ESP32 GPIO pin.

