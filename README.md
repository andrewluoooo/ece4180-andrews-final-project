# Andrews' ECE4180 Project 2

Skeeball Embedded Systems Implementation

Our final project is an implementation of skeeball as an embedded system with a microcontroller. Skee-Ball is a classic arcade game (at places like Dave & Buster’s) where players roll a ball up a ramp and aim to land it in a series of concentric rings, with smaller, harder-to-reach rings earning higher point values. At the end, there normally is a leaderboard that showcases individuals who got the highest aggregate point values. Overall, it will be a more portable version of the arcade game that is inspiring it.

The current very popular solution is the original implementation of skeeball. Skeeball machines that you see at arcades have a form factor in the magnitude of several feet. There are smaller skeeball machines that are marketed as portable, but they are either completely analog (just the physical ball launching mechanism) or completely digital (simulated ball launching mechanism based on user joystick input). We aim to take a hybrid approach to our version of skeeball, where the launching mechanism will be analog (retaining the satisfying part of the game) while also embedding some simple compute to enable a simple GUI with possible customizable settings, and track scores for both current session/all-time leaderboard. This gives the convenience of tracking through an embedded solution while also retaining the soul of the original game. Our implementation will have a leaderboard (using saving-information to non-volatile memory on the MCU). We will also have a larger game than these, but smaller than normal Skee-Ball.

# Updating the Image
export PATH="$HOME/.platformio/penv/bin:$PATH"
hash -r
python3 --version   # should show 3.13.x
pio run -e um_tinys3