#include "mastermind_impl.hpp"

namespace mastermind {

mastermind_t::data::data(const remotes_t &remotes, const std::shared_ptr<cocaine::framework::logger_t> &logger, int group_info_update_period)
	: m_logger(logger)
	, m_remotes(remotes)
	, m_next_remote(0)
	, m_group_info_update_period(group_info_update_period)
	, m_done(false)
{
	m_bad_groups_cache = std::make_shared<std::vector<std::vector<int>>>();
	m_symmetric_groups_cache = std::make_shared<std::map<int, std::vector<int>>>();
	m_cache_groups = std::make_shared<std::map<std::string, std::vector<int>>>();
	m_namespaces_settings_cache = std::make_shared<std::vector<namespace_settings_t>>();

	deserialize();

	try {
		reconnect();
	} catch (const std::exception &ex) {
		COCAINE_LOG_ERROR(m_logger, "libmastermind: reconnect: %s", ex.what());
	}

	m_weight_cache_update_thread = std::thread(std::bind(&mastermind_t::data::collect_info_loop, this));
}

mastermind_t::data::data(const std::string &host, uint16_t port, const std::shared_ptr<cocaine::framework::logger_t> &logger, int group_info_update_period)
	: m_logger(logger)
	, m_next_remote(0)
	, m_group_info_update_period(group_info_update_period)
	, m_done(false)
{
	m_bad_groups_cache = std::make_shared<std::vector<std::vector<int>>>();
	m_symmetric_groups_cache = std::make_shared<std::map<int, std::vector<int>>>();
	m_cache_groups = std::make_shared<std::map<std::string, std::vector<int>>>();
	m_namespaces_settings_cache = std::make_shared<std::vector<namespace_settings_t>>();

	deserialize();

	m_remotes.emplace_back(host, port);
	try {
		reconnect();
	} catch (const std::exception &ex) {
		COCAINE_LOG_ERROR(m_logger, "libmastermind: reconnect: %s", ex.what());
	}
	m_weight_cache_update_thread = std::thread(std::bind(&mastermind_t::data::collect_info_loop, this));
}

mastermind_t::data::~data() {
	try {
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			(void)lock;
			m_done = true;
			m_weight_cache_condition_variable.notify_one();
		}
		m_weight_cache_update_thread.join();
	} catch (const std::system_error &) {
	}
}

void mastermind_t::data::reconnect() {
	std::lock_guard<std::mutex> lock(m_reconnect_mutex);
	(void) lock;

	size_t end = m_next_remote;
	size_t index = m_next_remote;
	size_t size = m_remotes.size();

	do {
		auto &remote = m_remotes[index];
		try {
			m_service_manager = cocaine::framework::service_manager_t::create(
				cocaine::framework::service_manager_t::endpoint_t(remote.first, remote.second));

			auto g = m_service_manager->get_service_async<cocaine::framework::app_service_t>("mastermind");
			g.wait_for(std::chrono::seconds(4));
			if (g.ready() == false){
				COCAINE_LOG_ERROR(m_logger, "libmastermind: reconnect: cannot get app_service");
				m_service_manager.reset();
				index = (index + 1) % size;
				continue;
			}
			m_app = g.get();

			COCAINE_LOG_INFO(m_logger, "libmastermind: reconnect: connected to %s:%d", remote.first.c_str(), static_cast<int>(remote.second));

			m_current_remote = remote;
			m_next_remote = (index + 1) % size;
			return;
		} catch (const cocaine::framework::service_error_t &ex) {
			COCAINE_LOG_ERROR(m_logger, "libmastermind: reconnect: service_error: %s", ex.what());
		} catch (const std::exception &ex) {
			COCAINE_LOG_ERROR(m_logger, "libmastermind: reconnect: %s", ex.what());
		}

		index = (index + 1) % size;
	} while (index != end);

	m_current_remote = remote_t();
	m_service_manager.reset();
	COCAINE_LOG_ERROR(m_logger, "libmastermind: reconnect: cannot recconect to any host");
	throw std::runtime_error("reconnect error: cannot reconnect to any host");
}

bool mastermind_t::data::collect_group_weights() {
	try {
		metabalancer_groups_info_t::namespaces_t resp;

		retry(&cocaine::framework::app_service_t::enqueue<char [1]>, resp, "get_group_weights", "");

		{
			std::lock_guard<std::recursive_mutex> lock(m_weight_cache_mutex);
			(void) lock;
			m_metabalancer_groups_info = std::make_shared<metabalancer_groups_info_t>(std::move(resp));
		}

		return true;
	} catch(const std::exception &ex) {
		COCAINE_LOG_ERROR(m_logger, "libmastermind: collect_group_weights: %s", ex.what());
	}
	return false;
}

bool mastermind_t::data::collect_bad_groups() {
	try {
		auto cache = std::make_shared<std::vector<std::vector<int>>>();
		retry(&cocaine::framework::app_service_t::enqueue<char [1]>, *cache, "get_bad_groups", "");

		{
			std::lock_guard<std::recursive_mutex> lock(m_bad_groups_mutex);
			(void) lock;
			m_bad_groups_cache.swap(cache);
		}

		return true;
	} catch (const std::exception &ex) {
		COCAINE_LOG_ERROR(m_logger, "libmastermind: collect_bad_groups: %s", ex.what());
	}
	return false;
}

bool mastermind_t::data::collect_symmetric_groups() {
	try {
		std::vector<std::vector<int>> sym_groups;
		retry(&cocaine::framework::app_service_t::enqueue<char [1]>, sym_groups, "get_symmetric_groups", "");

		auto cache = std::make_shared<std::map<int, std::vector<int>>>();

		for (auto it = sym_groups.begin(); it != sym_groups.end(); ++it) {
			for (auto ve = it->begin(); ve != it->end(); ++ve) {
				(*cache)[*ve] = *it;
			}
		}

		for (auto it = m_bad_groups_cache->begin(); it != m_bad_groups_cache->end(); ++it) {
			for (auto ve = it->begin(); ve != it->end(); ++ve) {
				(*cache)[*ve] = *it;
			}
		}

		{
			std::lock_guard<std::recursive_mutex> lock(m_symmetric_groups_mutex);
			(void) lock;
			m_symmetric_groups_cache.swap(cache);
		}
		return true;
	} catch(const std::exception &ex) {
		COCAINE_LOG_ERROR(m_logger, "libmastermind: collect_symmetric_groups: %s", ex.what());
	}
	return false;
}

bool mastermind_t::data::collect_cache_groups() {
	try {
		std::vector<std::pair<std::string, std::vector<int>>> cache_groups;
		retry(&cocaine::framework::app_service_t::enqueue<char [1]>, cache_groups, "get_cached_keys", "");

		auto cache = std::make_shared<std::map<std::string, std::vector<int>>>();
		for (auto it = cache_groups.begin(); it != cache_groups.end(); ++it) {
			cache->insert(*it);
		}

		{
			std::lock_guard<std::recursive_mutex> lock(m_cache_groups_mutex);
			(void) lock;
			m_cache_groups.swap(cache);
		}
		return true;
	} catch(const std::exception &ex) {
		COCAINE_LOG_ERROR(m_logger, "libmastermind: collect_cache_groups: %s", ex.what());
	}
	return false;
}

bool mastermind_t::data::collect_namespaces_settings() {
	try {
		auto cache = std::make_shared<std::vector<namespace_settings_t>>();
		retry(&cocaine::framework::app_service_t::enqueue<char [1]>, *cache, "get_namespaces_settings", "");

		{
			std::lock_guard<std::recursive_mutex> lock(m_namespaces_settings_mutex);
			(void) lock;
			m_namespaces_settings_cache.swap(cache);
		}
		return true;
	} catch(const std::exception &ex) {
		COCAINE_LOG_ERROR(m_logger, "libmastermind: collect_namespaces_settings: %s", ex.what());
	}
	return false;
}

void mastermind_t::data::collect_info_loop() {
	std::unique_lock<std::mutex> lock(m_mutex);
#if __GNUC_MINOR__ >= 6
	auto no_timeout = std::cv_status::no_timeout;
	auto timeout = std::cv_status::timeout;
	auto tm = timeout;
#else
	bool no_timeout = true;
	bool timeout = false;
	bool tm = timeout;
#endif
	COCAINE_LOG_INFO(m_logger, "libmastermind: collect_info_loop: update period is %d", static_cast<int>(m_group_info_update_period));
	do {
		if (m_logger->verbosity() >= cocaine::logging::info) {
			auto current_remote = m_current_remote;
			std::ostringstream oss;
			oss << "libmastermind: collect_info_loop: begin; current host: ";
			if (current_remote.first.empty()) {
				oss << "none";
			} else {
				oss << current_remote.first << ':' << m_current_remote.second;
			}
			COCAINE_LOG_INFO(m_logger, "%s", oss.str().c_str());
		}

		auto beg_time = std::chrono::system_clock::now();

		{
			spent_time_printer_t helper("collect_group_weights", m_logger);
			collect_group_weights();
		}
		{
			spent_time_printer_t helper("collect_bad_groups", m_logger);
			collect_bad_groups();
		}
		{
			spent_time_printer_t helper("collect_symmetric_groups", m_logger);
			collect_symmetric_groups();
		}
		{
			spent_time_printer_t helper("collect_cache_groups", m_logger);
			collect_cache_groups();
		}
		{
			spent_time_printer_t helper("collect_namespaces_settings", m_logger);
			collect_namespaces_settings();
		}

		serialize();

		auto end_time = std::chrono::system_clock::now();

		if (m_logger->verbosity() >= cocaine::logging::info) {
			auto current_remote = m_current_remote;
			std::ostringstream oss;
			oss << "libmastermind: collect_info_loop: end; current host: ";
			if (current_remote.first.empty()) {
				oss << "none";
			} else {
				oss << current_remote.first << ':' << m_current_remote.second;
			}
			oss
				<< "; spent time: "
				<< std::chrono::duration_cast<std::chrono::milliseconds>(end_time - beg_time).count()
				<< " milliseconds";
			COCAINE_LOG_INFO(m_logger, "%s", oss.str().c_str());
		}

		tm = timeout;
		do {
			tm = m_weight_cache_condition_variable.wait_for(lock,
															std::chrono::seconds(
																m_group_info_update_period));
		} while(tm == no_timeout && m_done == false);
	} while(m_done == false);
}

void mastermind_t::data::serialize() {
	msgpack::sbuffer sbuf;
	msgpack::pack(&sbuf, std::make_tuple(
		*m_bad_groups_cache, *m_symmetric_groups_cache, *m_cache_groups, m_metabalancer_groups_info->data()
	));
	std::ofstream output("/var/tmp/libmastermind.cache");
	std::copy(sbuf.data(), sbuf.data() + sbuf.size(), std::ostreambuf_iterator<char>(output));
}

void mastermind_t::data::deserialize() {
	std::string file;
	{
		std::ifstream input("/var/tmp/libmastermind.cache");
		if (input.is_open() == false) {
			return;
		}
		typedef std::istreambuf_iterator<char> it_t;
		file.assign(it_t(input), it_t());
	}

	try {
		msgpack::unpacked msg;
		msgpack::unpack(&msg, file.data(), file.size());
		msgpack::object obj = msg.get();

		typedef std::tuple<
			 std::vector<std::vector<int>>
			, std::map<int, std::vector<int>>
			, std::map<std::string, std::vector<int>>
			, metabalancer_groups_info_t::namespaces_t> cache_type;
		cache_type ct;
		obj.convert(&ct);
		metabalancer_groups_info_t::namespaces_t namespaces;
		std::tie(*m_bad_groups_cache, *m_symmetric_groups_cache, *m_cache_groups, namespaces) = ct;
		m_metabalancer_groups_info = std::make_shared<metabalancer_groups_info_t>(std::move(namespaces));
	} catch (const std::exception &ex) {
		COCAINE_LOG_WARNING(m_logger, "libmastermind: deserialize: cannot read libmastermind.cache: %s", ex.what());
	} catch (...) {
		COCAINE_LOG_WARNING(m_logger, "libmastermind: deserialize: cannot read libmastermind.cache");
	}
}

} // namespace mastermind
