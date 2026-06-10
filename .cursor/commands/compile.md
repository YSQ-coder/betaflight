# compile

Write your command content here.

This command will be available in chat with /compile

build command:
make [target_name] -j[threads]

normally set thread to 100

if want to generate compile_commands.json by bear,use:
bear -- make [target_name] -j[threads]

however,it makes compile much slower,so only use it when need.