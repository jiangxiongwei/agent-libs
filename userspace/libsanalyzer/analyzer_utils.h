#pragma once

class sinsp_evttables;

///////////////////////////////////////////////////////////////////////////////
// Hashing support for stl pairs
///////////////////////////////////////////////////////////////////////////////
template <class T>
inline void hash_combine(std::size_t & seed, const T & v)
{
  std::hash<T> hasher;
  seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

namespace std
{
  template<typename S, typename T> struct hash<pair<S, T>>
  {
    inline size_t operator()(const pair<S, T> & v) const
    {
      size_t seed = 0;
      ::hash_combine(seed, v.first);
      ::hash_combine(seed, v.second);
      return seed;
    }
  };
}

///////////////////////////////////////////////////////////////////////////////
// Hashing support for ipv4tuple
// XXX for the moment, this has not been optimized for performance
///////////////////////////////////////////////////////////////////////////////
struct ip4t_hash
{
	size_t operator()(ipv4tuple t) const
	{
		size_t seed = 0;

		std::hash<uint64_t> hasher64;
		std::hash<uint32_t> hasher32;
		std::hash<uint8_t> hasher8;

		seed ^= hasher64(*(uint64_t*)t.m_all) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= hasher32(*(uint32_t*)(t.m_all + 8)) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= hasher8(*(uint8_t*)(t.m_all + 12)) + 0x9e3779b9 + (seed << 6) + (seed >> 2);

		return seed;
	}
};

struct ip4t_cmp
{
	bool operator () (ipv4tuple t1, ipv4tuple t2) const
	{
		return (memcmp(t1.m_all, t2.m_all, sizeof(t1.m_all)) == 0);
	}
};

///////////////////////////////////////////////////////////////////////////////
// Hashing support for unix_tuple
// not yet optimized
///////////////////////////////////////////////////////////////////////////////
struct unixt_hash
{
	size_t operator()(unix_tuple t) const
	{
		size_t seed = 0;

		std::hash<uint64_t> hasher64;

		seed ^= hasher64(*(uint64_t*)t.m_all) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= hasher64(*(uint64_t*)(t.m_all + 8)) + 0x9e3779b9 + (seed << 6) + (seed >> 2);

		return seed;
	}
};

struct unixt_cmp
{
	bool operator () (unix_tuple t1, unix_tuple t2) const
	{
		return (memcmp(t1.m_all, t2.m_all, sizeof(t1.m_all)) == 0);
	}
};

#ifdef SIMULATE_DROP_MODE
bool should_drop(sinsp_evt *evt);
#endif
