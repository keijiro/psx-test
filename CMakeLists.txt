cmake_minimum_required(VERSION 3.21)

project(
	psx-homebrew-hello
	LANGUAGES C
	VERSION 1.0.0
	DESCRIPTION "PSn00bSDK graphics, controller, and audio example"
)

psn00bsdk_add_executable(hello GPREL src/main.c)
