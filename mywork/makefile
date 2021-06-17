server: main.cpp threadpool.h  locker.h ./log/log.cpp ./log/log.h ./log/block_queue.h http_conn.cpp http_conn.h ./database/sql_connection_pool.cpp ./database/sql_connection_pool.h
	g++ -o server main.cpp threadpool.h locker.h ./log/log.cpp ./log/log.h http_conn.h http_conn.cpp ./database/sql_connection_pool.cpp ./database/sql_connection_pool.h -lpthread -lmysqlclient


clean:
	rm  -r server
