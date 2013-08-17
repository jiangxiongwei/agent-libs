#pragma once

#define CANCELED_FD_NUMBER MAXINT32
///////////////////////////////////////////////////////////////////////////////
// fd info
///////////////////////////////////////////////////////////////////////////////
class SINSP_PUBLIC sinsp_fdinfo
{
public:
	//
	// fd flags
	//
	enum flags
	{
		FLAGS_NONE = 0,
		FLAGS_TRANSACTION = (1 << 0),
		FLAGS_ROLE_CLIENT = (1 << 1),
		FLAGS_ROLE_SERVER = (1 << 2),
		FLAGS_CLOSE_IN_PROGRESS = (1 << 3),
		FLAGS_CLOSE_CANCELED = (1 << 4),
		// Pipe-specific flags
		FLAGS_IS_SOCKET_PIPE = (1 << 5),
	};

	sinsp_fdinfo();
	string* tostring();
	char get_typechar();

	scap_fd_type m_type;
	uint64_t m_create_time;
	uint32_t m_openflags;
	uint32_t m_flags;
	uint64_t m_ino;
	union
	{
		ipv4tuple m_ipv4info;
		ipv6tuple m_ipv6info;
		struct
		{
		  uint32_t m_ip;
		  uint16_t m_port;
		  uint8_t m_l4proto;
		} m_ipv4serverinfo;
		struct
		{
			uint32_t m_ip[4];
			uint16_t m_port;
			uint8_t m_l6proto;
		} m_ipv6serverinfo;
		unix_tuple m_unixinfo;
	}m_info;
	string m_name;

	bool is_unix_socket()
	{
		return m_type == SCAP_FD_UNIX_SOCK;
	}

	bool is_udp_socket()
	{
		return m_type == SCAP_FD_IPV4_SOCK && m_info.m_ipv4info.m_fields.m_l4proto == SCAP_L4_UDP;
	}

	bool is_tcp_socket()
	{
		return m_type == SCAP_FD_IPV4_SOCK && m_info.m_ipv4info.m_fields.m_l4proto == SCAP_L4_TCP;
	}

	bool is_ipv4_socket()
	{
		return m_type == SCAP_FD_IPV4_SOCK;
	}

	bool is_pipe()
	{
		return m_type == SCAP_FD_FIFO;
	}

	bool has_role_server()
	{
		return (m_flags & FLAGS_ROLE_SERVER) == FLAGS_ROLE_SERVER;
	}

	bool has_role_client()
	{
		return (m_flags & FLAGS_ROLE_CLIENT) == FLAGS_ROLE_CLIENT;
	}

	bool is_transaction()
	{
		return (m_flags & FLAGS_TRANSACTION) == FLAGS_TRANSACTION; 
	}

	bool is_role_none()
	{
		return m_flags == FLAGS_NONE;
	}

	void set_is_transaction()
	{
		m_flags |= sinsp_fdinfo::FLAGS_TRANSACTION;
	}

	void set_role_server()
	{
		m_flags |= sinsp_fdinfo::FLAGS_ROLE_SERVER;
	}

	void set_role_client()
	{
		m_flags |= sinsp_fdinfo::FLAGS_ROLE_CLIENT;
	}
        
	void reset_flags()
	{
		m_flags = FLAGS_NONE;
	}

	void set_socketpipe()
	{
		m_flags |= sinsp_fdinfo::FLAGS_IS_SOCKET_PIPE;
	}

	bool is_socketpipe()
	{
		return (m_flags & FLAGS_IS_SOCKET_PIPE) == FLAGS_IS_SOCKET_PIPE; 
	}

	void set_type_unix_socket()
	{
		m_type = SCAP_FD_UNIX_SOCK;
	}

	bool has_no_role()
	{
		return !has_role_client() && !has_role_server();
	}

	void print_on(FILE* f);

private:
	void add_filename(const char* directory, uint32_t directorylen, const char* filename, uint32_t filenamelen);

	friend class sinsp_parser;
	friend class sinsp_analyzer;
};

//
// fd operation
//
class sinsp_fdop
{
public:
	sinsp_fdop()
	{
	}

	sinsp_fdop(int64_t fd, uint16_t type)
	{
		m_fd = fd;
		m_type = type;
	}

	int64_t m_fd;
	uint16_t m_type;
};

///////////////////////////////////////////////////////////////////////////////
// fd info table
///////////////////////////////////////////////////////////////////////////////
//typedef fdtable_t::iterator fdtable_iterator_t;

class sinsp_fdtable
{
public:
	sinsp_fdtable(sinsp* inspector);
	sinsp_fdinfo* find(int64_t fd);
	// If the key is already present, overwrite the existing value and return false.
	void add(int64_t fd, sinsp_fdinfo* fdinfo);
	// If the key is present, returns true, otherwise returns false.
	void erase(int64_t fd);
	void clear();
	size_t size();
	void reset_cache();

	void print_on(FILE* f);

private:
	sinsp* m_inspector;
	unordered_map<int64_t, sinsp_fdinfo> m_fdtable;

	//
	// Simple fd cache
	//
	int64_t m_last_accessed_fd;
	sinsp_fdinfo *m_last_accessed_fdinfo;

	friend class sinsp_parser;
	friend class sinsp_thread_manager;
};