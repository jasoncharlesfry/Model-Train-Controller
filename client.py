# client.py
# Jason Fry
# Summer 2021

import socket
import sys  
import os
import select


host_ip = "192.168.1.19"
host_port = 3333
os.system('cls' if os.name == 'nt' else 'clear')
# create socket
try:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
except socket.error:
    print('Failed to create socket')
    sys.exit()

# Connect to remote server
print('# Connecting to server, ' + host_ip)
s.connect((host_ip , host_port))

# Send data to remote server


usr_str = '0'
os.system('cls' if os.name == 'nt' else 'clear')
bad_flag = 0
bad_input = 'bad'
while usr_str != 'q':
	s.send("0")
	if bad_flag == 0:
		print('Please wait')
	duty = s.recv(128)
	os.system('cls' if os.name == 'nt' else 'clear')
	if bad_flag == 1:
		print(bad_input + ' is not a valid entry.\n')
		bad_flag = 0;
	while sys.stdin in select.select([sys.stdin], [], [], 0)[0]:
		trash = raw_input();
	print('Current duty cycle is ' + duty + '%.')
	print('Enter new duty cycle between -100 and 100 or q to quit.')
	usr_str = raw_input('> ')
	os.system('cls' if os.name == 'nt' else 'clear')
	if usr_str == 'q':
		continue
		
	try:
		if int(usr_str) < -100 or int(usr_str) > 100:
			bad_flag = 1
			bad_input = usr_str
			continue
	except ValueError:
		bad_flag = 1
		bad_input = usr_str
		continue

	s.send('1 ' + usr_str + ' 0')

s.send ('1 0 0')
print('Goodbye!\n')