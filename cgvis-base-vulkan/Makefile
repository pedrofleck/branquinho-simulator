./bin/Linux/main: src/* include/* CMakeLists.txt CMakePresets.json
	cmake --preset default-config
	cmake --build --preset default-build

.PHONY: clean run
clean:
	rm -f bin/Linux/main
	rm -rf bin/shaders

run: ./bin/Linux/main
	cd bin/Linux && ./main
