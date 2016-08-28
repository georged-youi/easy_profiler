/************************************************************************
* file name         : reader.cpp
* ----------------- :
* creation time     : 2016/06/19
* copyright         : (c) 2016 Sergey Yagovtsev, Victor Zarubkin
* authors           : Sergey Yagovtsev, Victor Zarubkin
* emails            : yse.sey@gmail.com, v.s.zarubkin@gmail.com
* ----------------- :
* description       : The file contains implementation of fillTreesFromFile function
*                   : which reads profiler file and fill profiler blocks tree.
* ----------------- :
* change log        : * 2016/06/19 Sergey Yagovtsev: First fillTreesFromFile implementation.
*                   :
*                   : * 2016/06/25 Victor Zarubkin: Removed unnecessary memory allocation and copy
*                   :       when creating and inserting blocks into the tree.
*                   :
*                   : * 2016/06/26 Victor Zarubkin: Added statistics gathering (min, max, average duration,
*                   :       number of block calls).
*                   : * 2016/06/26 Victor Zarubkin, Sergey Yagovtsev: Added statistics gathering for root
*                   :       blocks in the tree.
*                   :
*                   : * 2016/06/29 Victor Zarubkin: Added calculaton of total children number for blocks.
*                   :
*                   : * 2016/06/30 Victor Zarubkin: Added this header.
*                   :       Added tree depth calculation.
*                   :
*                   : * 
* ----------------- :
* license           : TODO: add license text
************************************************************************/

#include "profiler/reader.h"
#include <fstream>
#include <iterator>
#include <algorithm>
#include <unordered_map>
#include <thread>

//////////////////////////////////////////////////////////////////////////

struct passthrough_hash {
    template <class T> inline size_t operator () (T _value) const {
        return static_cast<size_t>(_value);
    }
};

//////////////////////////////////////////////////////////////////////////

namespace profiler {

    void SerializedData::set(char* _data)
    {
        if (m_data != nullptr)
            delete[] m_data;
        m_data = _data;
    }

    extern "C" void release_stats(BlockStatistics*& _stats)
    {
        if (!_stats)
        {
            return;
        }

        if (--_stats->calls_number == 0)
        {
            delete _stats;
        }

        _stats = nullptr;
    }

}

//////////////////////////////////////////////////////////////////////////

#ifdef _WIN32

/** \brief Simple C-string pointer with length.

It is used as base class for a key in std::unordered_map.
It is used to get better performance than std::string.
It simply stores a pointer and a length, there is no
any memory allocation and copy.

\note It is absolutely safe to store pointer because std::unordered_map,
which uses it as a key, exists only inside fillTreesFromFile function.

*/
class cstring
{
protected:

    const char* str;
    size_t  str_len;

public:

    explicit cstring(const char* _str) : str(_str), str_len(strlen(_str))
    {
    }

    cstring(const cstring& _other) : str(_other.str), str_len(_other.str_len)
    {
    }

    inline bool operator == (const cstring& _other) const
    {
        return str_len == _other.str_len && !strncmp(str, _other.str, str_len);
    }

    inline bool operator != (const cstring& _other) const
    {
        return !operator == (_other);
    }

    inline bool operator < (const cstring& _other) const
    {
        if (str_len == _other.str_len)
        {
            return strncmp(str, _other.str, str_len) < 0;
        }

        return str_len < _other.str_len;
    }
};

/** \brief cstring with precalculated hash.

This is used to calculate hash for C-string and to cache it
to be used in the future without recurring hash calculatoin.

\note This class is used as a key in std::unordered_map.

*/
class hashed_cstr : public cstring
{
    typedef cstring Parent;

public:

    size_t str_hash;

    explicit hashed_cstr(const char* _str) : Parent(_str), str_hash(0)
    {
        str_hash = ::std::_Hash_seq((const unsigned char *)str, str_len);
    }

    hashed_cstr(const hashed_cstr& _other) : Parent(_other), str_hash(_other.str_hash)
    {
    }

    inline bool operator == (const hashed_cstr& _other) const
    {
        return str_hash == _other.str_hash && Parent::operator == (_other);
    }

    inline bool operator != (const hashed_cstr& _other) const
    {
        return !operator == (_other);
    }
};

namespace std {

    /** \brief Simply returns precalculated hash of a C-string. */
    template <> struct hash<hashed_cstr> {
        inline size_t operator () (const hashed_cstr& _str) const {
            return _str.str_hash;
        }
    };

}

typedef ::std::unordered_map<::profiler::block_id_t, ::profiler::BlockStatistics*, passthrough_hash> StatsMap;
typedef ::std::unordered_map<hashed_cstr, ::profiler::block_id_t> IdMap;

#else

// TODO: optimize for Linux too
#include <string>
typedef ::std::unordered_map<::profiler::block_id_t, ::profiler::BlockStatistics*, passthrough_hash> StatsMap;
typedef ::std::unordered_map<::std::string, ::profiler::block_id_t> IdMap;

#endif

//////////////////////////////////////////////////////////////////////////

/** \brief Updates statistics for a profiler block.

\param _stats_map Storage of statistics for blocks.
\param _current Pointer to the current block.
\param _stats Reference to the variable where pointer to the block statistics must be written.

\note All blocks with similar name have the same pointer to statistics information.

\note As all profiler block keeps a pointer to it's statistics, all similar blocks
automatically receive statistics update.

*/
::profiler::BlockStatistics* update_statistics(StatsMap& _stats_map, const ::profiler::BlocksTree& _current)
{
    auto duration = _current.node->duration();
    //StatsMap::key_type key(_current.node->name());
    //auto it = _stats_map.find(key);
    auto it = _stats_map.find(_current.node->id());
    if (it != _stats_map.end())
    {
        // Update already existing statistics

        auto stats = it->second; // write pointer to statistics into output (this is BlocksTree:: per_thread_stats or per_parent_stats or per_frame_stats)

        ++stats->calls_number; // update calls number of this block
        stats->total_duration += duration; // update summary duration of all block calls

        if (duration > stats->max_duration)
        {
            // update max duration
            stats->max_duration_block = _current.block_index;
            stats->max_duration = duration;
        }

        if (duration < stats->min_duration)
        {
            // update min duraton
            stats->min_duration_block = _current.block_index;
            stats->min_duration = duration;
        }

        // average duration is calculated inside average_duration() method by dividing total_duration to the calls_number

        return stats;
    }

    // This is first time the block appear in the file.
    // Create new statistics.
    auto stats = new ::profiler::BlockStatistics(duration, _current.block_index);
    //_stats_map.emplace(key, stats);
    _stats_map.emplace(_current.node->id(), stats);

    return stats;
}

//////////////////////////////////////////////////////////////////////////

void update_statistics_recursive(StatsMap& _stats_map, ::profiler::BlocksTree& _current)
{
    _current.per_frame_stats = update_statistics(_stats_map, _current);
    for (auto& child : _current.children)
    {
        update_statistics_recursive(_stats_map, child);
    }
}

//////////////////////////////////////////////////////////////////////////

typedef ::std::map<::profiler::thread_id_t, StatsMap> PerThreadStats;

extern "C" {

    unsigned int fillTreesFromFile(::std::atomic<int>& progress, const char* filename, ::profiler::SerializedData& serialized_blocks, ::profiler::SerializedData& serialized_descriptors, ::profiler::descriptors_list_t& descriptors, ::profiler::thread_blocks_tree_t& threaded_trees, bool gather_statistics)
	{
        PROFILER_BEGIN_FUNCTION_BLOCK_GROUPED(::profiler::colors::Cyan)

		::std::ifstream inFile(filename, ::std::fstream::binary);
        progress.store(0);

		if (!inFile.is_open())
            return 0;

        PerThreadStats thread_statistics, parent_statistics, frame_statistics;
		unsigned int blocks_counter = 0;

        uint32_t total_blocks_number = 0;
        inFile.read((char*)&total_blocks_number, sizeof(decltype(total_blocks_number)));
        if (total_blocks_number == 0)
            return 0;

        uint64_t memory_size = 0;
        inFile.read((char*)&memory_size, sizeof(decltype(memory_size)));
        if (memory_size == 0)
            return 0;

        serialized_blocks.set(new char[memory_size]);
        //memset(serialized_blocks[0], 0, memory_size);


        uint32_t total_descriptors_number = 0;
        inFile.read((char*)&total_descriptors_number, sizeof(decltype(total_descriptors_number)));
        if (total_descriptors_number == 0)
            return 0;

        uint64_t descriptors_memory_size = 0;
        inFile.read((char*)&descriptors_memory_size, sizeof(decltype(descriptors_memory_size)));
        if (descriptors_memory_size == 0)
            return 0;

        descriptors.reserve(total_descriptors_number);
        serialized_descriptors.set(new char[descriptors_memory_size]);

        uint64_t i = 0;
        uint32_t read_number = 0;
        while (!inFile.eof() && read_number < total_descriptors_number)
        {
            ++read_number;

            uint16_t sz = 0;
            inFile.read((char*)&sz, sizeof(sz));
            if (sz == 0)
                return 0;

            char* data = serialized_descriptors[i];
            inFile.read(data, sz);
            //auto base_offset = data;
            //inFile.read(data, sizeof(::profiler::BaseBlockDescriptor));
            //auto base = reinterpret_cast<::profiler::BaseBlockDescriptor*>(data);
            //data = data + sizeof(::profiler::BaseBlockDescriptor);
            //inFile.read(data, sizeof(uint16_t));
            //auto name_len = reinterpret_cast<uint16_t*>(data);
            //data = data + sizeof(uint16_t);
            //inFile.read(data, *name_len);
            //data = data + *name_len;
            //inFile.read(data, sz - ::std::distance(base_offset, data));
            i += sz;
            auto descriptor = reinterpret_cast<::profiler::SerializedBlockDescriptor*>(data);
            descriptors.push_back(descriptor);

            progress.store(static_cast<int>(10 * i / descriptors_memory_size));
        }

        IdMap identification_table;

        i = 0;
        read_number = 0;
        while (!inFile.eof() && read_number < total_blocks_number)
        {
            PROFILER_BEGIN_BLOCK_GROUPED("Read thread from file", ::profiler::colors::Darkgreen)

            ::profiler::thread_id_t thread_id = 0;
            inFile.read((char*)&thread_id, sizeof(decltype(thread_id)));

            uint32_t blocks_number_in_thread = 0;
            inFile.read((char*)&blocks_number_in_thread, sizeof(decltype(blocks_number_in_thread)));

            auto& root = threaded_trees[thread_id];
            const auto threshold = read_number + blocks_number_in_thread;
            while (!inFile.eof() && read_number < threshold)
            {
                PROFILER_BEGIN_BLOCK_GROUPED("Read block from file", ::profiler::colors::Green)

                ++read_number;

                uint16_t sz = 0;
                inFile.read((char*)&sz, sizeof(sz));
                if (sz == 0)
                    return 0;

                char* data = serialized_blocks[i];
                inFile.read(data, sz);
                i += sz;
                auto baseData = reinterpret_cast<::profiler::SerializedBlock*>(data);

                ::profiler::BlocksTree tree;
                tree.node = baseData;// new ::profiler::SerializedBlock(sz, data);
                tree.block_index = blocks_counter++;

                auto& per_parent_statistics = parent_statistics[thread_id];
                auto& per_thread_statistics = thread_statistics[thread_id];
                auto descriptor = descriptors[baseData->id()];

                if (descriptor->type() == ::profiler::BLOCK_TYPE_THREAD_SIGN)
                {
                    root.thread_name = tree.node->name();
                }

                if (*tree.node->name() != 0)
                {
                    IdMap::key_type key(tree.node->name());
                    auto it = identification_table.find(key);
                    if (it != identification_table.end())
                    {
                        baseData->setId(it->second);
                    }
                    else
                    {
                        auto id = static_cast<::profiler::block_id_t>(descriptors.size());
                        identification_table.emplace(key, id);
                        descriptors.push_back(descriptors[baseData->id()]);
                        baseData->setId(id);
                    }
                }

                if (!root.tree.children.empty())
                {
                    auto& back = root.tree.children.back();
                    auto t1 = back.node->end();
                    auto mt0 = tree.node->begin();
                    if (mt0 < t1)//parent - starts earlier than last ends
                    {
                        //auto lower = ::std::lower_bound(root.children.begin(), root.children.end(), tree);
                        /**/
                        PROFILER_BEGIN_BLOCK_GROUPED("Find children", ::profiler::colors::Blue)
                        auto rlower1 = ++root.tree.children.rbegin();
                        for (; rlower1 != root.tree.children.rend(); ++rlower1)
                        {
                            if (mt0 > rlower1->node->begin())
                            {
                                break;
                            }
                        }
                        auto lower = rlower1.base();
                        ::std::move(lower, root.tree.children.end(), ::std::back_inserter(tree.children));

                        root.tree.children.erase(lower, root.tree.children.end());
                        PROFILER_END_BLOCK

                        ::profiler::timestamp_t children_duration = 0;
                        if (gather_statistics)
                        {
                            PROFILER_BEGIN_BLOCK_GROUPED("Gather statistic within parent", ::profiler::colors::Magenta)
                            per_parent_statistics.clear();

                            //per_parent_statistics.reserve(tree.children.size());     // this gives slow-down on Windows
                            //per_parent_statistics.reserve(tree.children.size() * 2); // this gives no speed-up on Windows
                            // TODO: check this behavior on Linux

                            for (auto& child : tree.children)
                            {
                                child.per_parent_stats = update_statistics(per_parent_statistics, child);

                                children_duration += child.node->duration();
                                if (tree.depth < child.depth)
                                    tree.depth = child.depth;
                            }
                        }
                        else
                        {
                            for (const auto& child : tree.children)
                            {
                                children_duration += child.node->duration();
                                if (tree.depth < child.depth)
                                    tree.depth = child.depth;
                            }
                        }

                        ++tree.depth;
                    }
                }

                root.tree.children.emplace_back(::std::move(tree));



                if (gather_statistics)
                {
                    PROFILER_BEGIN_BLOCK_GROUPED("Gather per thread statistics", ::profiler::colors::Coral)
                    auto& current = root.tree.children.back();
                    current.per_thread_stats = update_statistics(per_thread_statistics, current);
                }

                if (progress.load() < 0)
                    break;
                progress.store(10 + static_cast<int>(80 * i / memory_size));
            }
		}

        if (progress.load() < 0)
        {
            serialized_blocks.clear();
            threaded_trees.clear();
            return 0;
        }

        PROFILER_BEGIN_BLOCK_GROUPED("Gather statistics for roots", ::profiler::colors::Purple)
        if (gather_statistics)
		{
            ::std::vector<::std::thread> statistics_threads;
            statistics_threads.reserve(threaded_trees.size());

			for (auto& it : threaded_trees)
			{
                auto& root = it.second;
                root.thread_id = it.first;
                root.tree.shrink_to_fit();

                auto& per_frame_statistics = frame_statistics[root.thread_id];
                auto& per_parent_statistics = parent_statistics[it.first];
				per_parent_statistics.clear();

                statistics_threads.emplace_back(::std::thread([&per_parent_statistics, &per_frame_statistics](::profiler::BlocksTreeRoot& root)
                {
                    for (auto& frame : root.tree.children)
                    {
                        frame.per_parent_stats = update_statistics(per_parent_statistics, frame);

                        per_frame_statistics.clear();
                        update_statistics_recursive(per_frame_statistics, frame);

                        if (root.tree.depth < frame.depth)
                            root.tree.depth = frame.depth;
                    }

                    ++root.tree.depth;
                }, ::std::ref(root)));
			}

            int j = 0, n = static_cast<int>(statistics_threads.size());
            for (auto& t : statistics_threads)
            {
                t.join();
                progress.store(90 + (10 * ++j) / n);
            }
		}
        else
        {
            int j = 0, n = static_cast<int>(threaded_trees.size());
            for (auto& it : threaded_trees)
            {
                auto& root = it.second;
                root.thread_id = it.first;

                root.tree.shrink_to_fit();
                for (auto& frame : root.tree.children)
                {
                    if (root.tree.depth < frame.depth)
                        root.tree.depth = frame.depth;
                }

                ++root.tree.depth;

                progress.store(90 + (10 * ++j) / n);
            }
        }
        PROFILER_END_BLOCK
        // No need to delete BlockStatistics instances - they will be deleted inside BlocksTree destructors

        return blocks_counter;
	}
}
