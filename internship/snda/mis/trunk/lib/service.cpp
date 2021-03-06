#include "service.h"
#include "helper.h"
#include "memcachedst.h"
#include <stdio.h>

#define PING_TIMEOUT 600
#define MAX_RETRY 3

MISService::MISService(MISConfig& config) : m_db_conn(false)  {
	m_config = config;
	m_last_ping_time = 0;
	m_connection_error = false;
}

MISService::~MISService(){
	clear();
	if (m_db_conn.connected ()) m_db_conn.disconnect();
}

void MISService::clear() {
	for (MISChannelList::iterator iter = m_channels.begin(); iter != m_channels.end(); ++iter) {
		delete *iter;
	}
	m_channels.clear();
}

bool MISService::checkDBConnection() {
	unsigned long now = time (NULL);
	if (now - m_last_ping_time >= PING_TIMEOUT || m_last_ping_time > now) {
		m_last_ping_time = now;
		if (m_db_conn.ping()) return true;
	} else {
		return true;
	}
	// ping failed
	//if (m_db_conn.connected ()) return true;
	string host = m_config["db.host"];
	string user = m_config["db.user"];
	string password = m_config["db.password"];
	string port = m_config["db.port"];
	int iport = 0;
	if (!port.empty()) iport = atoi(port.c_str());
	if (!m_db_conn.connect("d_mis", host.c_str(), user.c_str(), password.c_str(), iport)) {	
			cerr << "DB connection failed: " << m_db_conn.error() << endl;
			m_last_ping_time = 0;
			return false;
	}
	try {
		mysqlpp::Query query = m_db_conn.query(); 
		query << "SET NAMES 'utf8'"; 
		query.execute();
	} catch (...) {
		cerr << "Unhandled Exception" << endl;
		m_last_ping_time = 0;
		return false;
	}
	return true;
}

bool MISService::loadFromDB() {
	for (int i = 0; i < MAX_RETRY; ++i) {
		if (i > 0) cerr << "retry to call loadFromDB: " << i << endl;
		if (_loadFromDB()) return true;
	}
	return false;
}

bool MISService::_loadFromDB() {
	try {
		if (!checkDBConnection()) return false;

		// 加载频道
		mysqlpp::Query query0 = m_db_conn.query("select channel_id, name from d_mis.t_mis_channel");
		mysqlpp::StoreQueryResult res0 = query0.store();
		if (!res0) {
			cerr << "Failed to get stock table: " << query0.error() << endl;
			m_last_ping_time = 0;
			return false;
		}

		// 请空原来数据
		clear();

		for (size_t i = 0; i < res0.num_rows(); ++i) {
			getChannel(res0[i]["channel_id"].c_str(), true);
		}

		// 加载词典
		mysqlpp::Query query = m_db_conn.query("select dict_id, mode, type, channel from d_mis.t_mis_dict");
		mysqlpp::StoreQueryResult res = query.store();
		if (!res) {
			cerr << "Failed to get stock table: " << query.error() << endl;
			m_last_ping_time = 0;
			return false;
		}

		string channel;
		string pattern;
		int type;
		int mode;
		int dict_id;
		int timeout;
		int times;
		int id;
		MISChannel* pchannel;
		MISDict* pdict;
		string sql;

		for (size_t i = 0; i < res.num_rows(); ++i) {
			channel = res[i]["channel"].c_str();
			type =  atoi(res[i]["type"].c_str());
			mode =  atoi(res[i]["mode"].c_str());
			dict_id =  atoi(res[i]["dict_id"].c_str());
			pchannel = getChannel(channel, true);
			pdict = pchannel->addDict(dict_id, type, mode);
			if (pdict) {
				sql = "select value from d_mis.t_mis_pattern where dict_id=";
				sql += res[i]["dict_id"].c_str();
				mysqlpp::Query query2 = m_db_conn.query(sql.c_str());
				mysqlpp::StoreQueryResult res2 = query2.store();
				if (!res2) {
					cerr << "Failed to get stock table: " << query2.error() << endl;
					return false;
				}
				for (size_t j = 0; j < res2.num_rows(); ++j) {
					pattern =  res2[j]["value"].c_str();
					transform(pattern.begin(), pattern.end(), pattern.begin(), ::tolower);
					pdict->addPattern(pattern);
				}
			}
		}

		// 初始化memcache
		bool has_memcache = false;
		mysqlpp::Query query3 = m_db_conn.query("select value from d_mis.t_mis_config where name='memcached.servers'");
		mysqlpp::StoreQueryResult res3 = query3.store();
		if (!res) {
			cerr << "Failed to get stock table1: " << query3.error() << endl;
			m_last_ping_time = 0;
			return false;
		}

		if (res3.num_rows() > 0) {
			bool ret = MISMemcachedST::init(res3[0]["value"].c_str());
			has_memcache = true;
			if (ret == false) {
				printf("------------------------------WARNING------------------------------\ncan not connect memcache with servers:\n  %s\nplease check\n  %s\n", res3[0]["value"].c_str(), "http://aqyw.sdo.com/projects/uc/wiki/%E4%BF%A1%E6%81%AF%E5%AE%A1%E6%A0%B8MIS");
			}
		} else {
			printf("------------------------------WARNING------------------------------\nthere is no memcached configed, system will ignore frequency limiter\nfor more details, please check\n  %s\n", "http://aqyw.sdo.com/projects/uc/wiki/%E4%BF%A1%E6%81%AF%E5%AE%A1%E6%A0%B8MIS");
		}

		if (has_memcache) {
			mysqlpp::Query query4 = m_db_conn.query("select frequency_limit_id, channel, type, timeout, times from d_mis.t_mis_frequency");
			mysqlpp::StoreQueryResult res4 = query4.store();
			if (!res4) {
				cerr << "Failed to get stock table: " << query4.error() << endl;
				m_last_ping_time = 0;
				return false;
			}
			
			for (size_t i = 0; i < res4.num_rows(); ++i) {
				channel = res4[i]["channel"].c_str();
				type = atoi(res4[i]["type"].c_str());
				timeout = atoi(res4[i]["timeout"].c_str());
				times = atoi(res4[i]["times"].c_str());
				id = atoi(res4[i]["frequency_limit_id"].c_str());
				pchannel = getChannel(channel, true);
				pchannel->addFrequencyLimiter(id, type, (time_t)timeout, times);
			}

			string consumer_key,interfaceName;
			mysqlpp::Query query5 = m_db_conn.query("select id, channel, type, consumer_key, interfacename ,timeout, times from d_mis.t_mis_interface_frequency");
			mysqlpp::StoreQueryResult res5 = query5.store();
			if (!res5) {
				cerr << "Failed to get stock table: " << query5.error() << endl;
				m_last_ping_time = 0;
				return false;
			}
			for(size_t j = 0 ; j < res5.num_rows();++j)
			{
				channel = res5[ j]["channel"].c_str();
				type = atoi(res5[ j]["type"].c_str());
				timeout = atoi(res5[ j]["timeout"].c_str());
				times = atoi(res5[j]["times"].c_str());
				consumer_key  = res5[ j]["consumer_key"].c_str();
				interfaceName  = res5[ j]["interfacename"].c_str();
				pchannel = getChannel(channel, true);
				pchannel->addInterfaceFrequencyLimiter(type,(time_t)timeout,times,consumer_key,interfaceName);
			}
		}

		return true;
	} catch (...) {
		cerr << "Unhandled Exception" << endl;
		m_last_ping_time = 0;
		return false;
	}
	return true;
}

MISChannel* MISService::getChannel(const string& name, bool create) {
	for (MISChannelList::iterator iter = m_channels.begin(); iter != m_channels.end(); ++iter) {
		if ((*iter)->m_name == name) return *iter;
	}
	if (create) {
		MISChannel* pchannel = new MISChannel(name.c_str());
		m_channels.push_back(pchannel);
		return pchannel;
	}
	return NULL;
}

bool MISService::addToQueue(MISInput& input, MISOutput& output) {
	for (int i = 0; i < MAX_RETRY; ++i) {
		if (i > 0) cerr << "retry to call _addToQueue: " << i << endl;
		if (_addToQueue(input, output)) return true;
	}
	return false;
}

bool MISService::_addToQueue(MISInput& input, MISOutput& output) {
	try {
		if (!checkDBConnection()) return false;
		Query query = m_db_conn.query();
        query << "INSERT INTO d_mis.t_mis_queue (user_id, channel, ip, content, record_id, reason, add_time) VALUES("
				<< "'" << input.user_id.c_str()
				<< "', '" << mysqlpp::escape << input.channel
				<< "', '" << mysqlpp::escape << input.ip
				<< "', '" << mysqlpp::escape << input.content
				<< "', '" << mysqlpp::escape << input.record_id
				<< "', '";
		
		if (output.m_matched_result) {
			bool started = false;
			for (MISMatchedResult::iterator iter = output.m_matched_result->begin(); iter != output.m_matched_result->end(); ++iter) {
				if (started) query << "\n";
				else started = true;
				query << iter->first << ":" << mysqlpp::escape << iter->second;
			}
		}
		query << "', unix_timestamp())";
        query.execute();
	} catch (...) {
		cerr << "Unhandled Exception" << endl;
		m_last_ping_time = 0;
		return false;
	}
	return true;
}

bool MISService::addToQueue(MISInput& input, const string& reason) {
	for (int i = 0; i < MAX_RETRY; ++i) {
		if (i > 0) cerr << "retry to call _addToQueue2: " << i << endl;
		if (_addToQueue(input, reason)) return true;
	}
	return false;
}

bool MISService::_addToQueue(MISInput& input, const string& reason) {
	try {
		if (!checkDBConnection()) return false;
		Query query = m_db_conn.query();
        query << "INSERT INTO d_mis.t_mis_queue (user_id, channel, ip, content, record_id, reason, add_time) VALUES("
				<< "'" << input.user_id.c_str()
				<< "', '" << mysqlpp::escape << input.channel
				<< "', '" << mysqlpp::escape << input.ip
				<< "', '" << mysqlpp::escape << input.content
				<< "', '" << mysqlpp::escape << input.record_id
				<< "', '" << mysqlpp::escape << reason
				<< "', unix_timestamp())";
        query.execute();
	} catch (...) {
		cerr << "Unhandled Exception" << endl;
		m_last_ping_time = 0;
		return false;
	}
	return true;
}

bool MISService::addToLog(MISInput& input, MISOutput& output) {
	for (int i = 0; i < MAX_RETRY; ++i) {
		if (i > 0) cerr << "retry to call _addToLog: " << i << endl;
		if (_addToLog(input, output)) return true;
	}
	return false;
}

bool MISService::_addToLog(MISInput& input, MISOutput& output) {
	try {
		if (!checkDBConnection()) return false;
		Query query = m_db_conn.query();
        query << "INSERT INTO d_mis.t_mis_log (user_id, channel, ip, content, record_id, result, reason, add_time) VALUES("
				<< "'" << input.user_id.c_str()
				<< "', '" << mysqlpp::escape << input.channel
				<< "', '" << mysqlpp::escape << input.ip
				<< "', '" << mysqlpp::escape << input.content
				<< "', '" << mysqlpp::escape << input.record_id
				<< "', '" << mysqlpp::escape << output.m_result
				<< "', '";
		if (output.m_matched_result) {
			bool started = false;
			for (MISMatchedResult::iterator iter = output.m_matched_result->begin(); iter != output.m_matched_result->end(); ++iter) {
				if (started) query << "\n";
				else started = true;
				query << iter->first << ":" << mysqlpp::escape << iter->second;
			}
		}
		query << "', unix_timestamp())";
        query.execute();
	} catch (...) {
		cerr << "Unhandled Exception" << endl;		
		m_last_ping_time = 0;
		return false;
	}
	return true;
}

void MISService::check(MISInput& input, MISOutput& output) {
	MISChannel* pchannel = getChannel(input.channel);
	if (pchannel) {
		pchannel->check(input, output);
		if (input.record_id.empty()) {
			// 只记录BAD的情况
			if (MISRESULT_BAD == output.m_result)
				addToLog(input, output);
		} else {
			addToLog(input, output);
			if (MISRESULT_QUEUE == output.m_result) {
				addToQueue(input, output);
			}
		}
	} else {
		output.m_result = MISRESULT_BAD_REQUEST;
	}
}

#ifdef _RUN_MISSERVICE_
int main(int argc, char **argv) {
	if (argc == 1) {
		printf("usage: %s [config file]\n", argv[0]);
		return 0;
	}
	map<string, string> config;
	if (!MISHelper::readConfig(argv[1], config)) {
		printf("bad config file: %s\n", argv[1]);
		return 0;
	}


	MISService service(config);
	service.loadFromDB();
	MISInput input;
	MISOutput output;
	input.user_id = "100";
	input.channel = "twitter";
	input.content = "fl爱妹taobao妹g测fldg6d4试";
	input.record_id = "0123456789abcdef0123456789abcdef";

	service.check(input, output);
	/*output.m_result = MISRESULT_QUEUE;
	if (output.m_result == MISRESULT_QUEUE) {
		input.record_id = "0123456789abcdef0123456789abcdef";
		service.check(input, output);
	}*/
	printf("result:%d\n", output.m_result);
	if (output.m_matched_result) {
		for (MISMatchedResult::iterator iter = output.m_matched_result->begin(); iter != output.m_matched_result->end(); ++iter) {
			printf("matched:%d %s\n", iter->first, iter->second.c_str());
		}
	}
}
#endif
