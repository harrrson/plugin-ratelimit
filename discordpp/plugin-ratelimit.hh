#pragma once

#include <queue>
#include <map>
#include <set>
#include <algorithm>

#include <discordpp/log.hh>

namespace discordpp{
	using route_t = std::size_t;
	
	template<class BASE>
	class PluginRateLimit: public BASE, virtual BotStruct{
		struct Call;
		using QueueByRoute = std::map<route_t, std::queue<sptr<Call>>>;
	public:
		// The Discord API *typically* limits to 5 calls so use that for unknown buckets
		int defaultLimit = 5;
		
		// Intercept calls
		virtual void call(
			sptr<const std::string> requestType,
			sptr<const std::string> targetURL,
			sptr<const json> body,
			sptr<const std::function<void()>> onWrite,
			sptr<const std::function<void(const json)>> onRead
		) override{
			log::log(
				log::trace, [targetURL, body](std::ostream *log){
					*log << "Plugin: RateLimit: " << "Intercepted " << *targetURL << (body ? body->dump(4) : "{}")
					     << '\n';
				}
			);
			
			auto the_call = std::make_shared<Call>(Call{
				requestType,
				targetURL,
				getLimitedRoute(*targetURL),
				body,
				onWrite,
				onRead
			});
			
			std::cerr << "Hash " << the_call->route << '\n';
			
			(route_to_bucket.count(the_call->route)
				? buckets[route_to_bucket[the_call->route]].queues
				: queues
			)[the_call->route].push(the_call);
			
			// Kickstart the send loop
			aioc->post([this]{do_some_work();});
		}
	
	private:
		void do_some_work(){
			// If already looping don't start a new loop
			if(writing) return;
			writing = true;
			
			Bucket* next_bucket = nullptr;
			QueueByRoute* next_queues = nullptr;
			CountedSet<route_t>* next_transit = nullptr;
			route_t next_route = 0;
			int min_remaining = defaultLimit;
			std::time_t min = std::numeric_limits<std::time_t>::max();
			for(auto& be : buckets){
				if(route_to_bucket.count(gateway_route) && route_to_bucket[gateway_route] == be.second.id) continue;
				assert(be.second.remaining >= be.second.transit.total() && "More messages in transit than remaining in a bucket");
				min_remaining = std::min(min_remaining, int(be.second.remaining - be.second.transit.total()));
				if(be.second.remaining <= be.second.transit.total()) continue;
				for(auto& qe : be.second.queues){
					assert(!qe.second.empty() && "Encountered an empty queue in a bucket");
					auto created = qe.second.front()->created;
					if(created < min){
						min = created;
						next_bucket = &be.second;
						next_queues = &be.second.queues;
						next_transit = &be.second.transit;
						next_route = qe.first;
					}
				}
			}
			
			// `queues` may be empty
			for(auto& qe : queues){
				assert(!qe.second.empty() && "Encountered an empty global queue");
				auto created = qe.second.front()->created;
				if(created < min){
					min = created;
					next_bucket = nullptr;
					next_queues = &queues;
					next_transit = &transit;
					next_route = qe.first;
				}
			}
			
			// Can we send a message?
			// If we found a next bucket, yes.
			// If the uncategorized queue isn't empty and there's no chance of overflowing a bucket, yes.
			if(!next_bucket && (queues.empty() || min_remaining <= transit.total())){
				writing = false;
				return;
			}
			
			// Get the next call and delete its queue if empty
			auto next_queue = next_queues->find(next_route);
			auto call = next_queue->second.front();
			next_queue->second.pop();
			if(next_queue->second.empty()){
				next_queues->erase(next_route);
			}
			
			log::log(
				log::trace, [call](std::ostream *log){
					*log << "Plugin: RateLimit: " << "Sending " << *call->targetURL << (call->body ? call->body->dump(4) : "{}")
					     << '\n';
				}
			);
			
			// Do the call
			// Note: We are binding raw pointers and we must guarantee their lifetimes
			BASE::call(
				call->requestType, call->targetURL, call->body,
				std::make_shared<std::function<void()>>(
					[this, route = next_route, call](){ // When the call is sent
						(route_to_bucket.count(route)
							? buckets[route_to_bucket[route]].transit
							: transit
						).insert(route);
						writing = false;
						
						// Check if the cycle continues
						aioc->post([this]{do_some_work();});
						
						// Run the user's onWrite callback
						if(call->onWrite != nullptr){
							(*call->onWrite)();
						}
					}
				),
				std::make_shared<std::function<void(const json msg)>>(
					[this, route = next_route, call](const json& msg){ // When Discord replies
						auto* bucket = (
							route_to_bucket.count(route)
							? &buckets[route_to_bucket[route]]
							: nullptr
						);
						auto& headers = msg["header"];
						
						(bucket ? bucket->transit : transit).erase(route);
						
						{
							auto new_id = headers["X-RateLimit-Bucket"].get<std::string>();
							if(!bucket || bucket->id != new_id){
								auto* old_bucket = bucket;
								
								route_to_bucket[route] = new_id;
								std::cerr << buckets.count(new_id) << '\n';
								bucket = &buckets.emplace(new_id, Bucket{new_id}).first->second;
								
								std::cerr << "Migrating from " << (old_bucket ? old_bucket->id : "global") << " to " << bucket->id << '\n';
								std::cerr << bucket->queues.count(route) << '\n';
								
								bucket->queues.insert((old_bucket ? old_bucket->queues : queues).extract(route));
								(old_bucket ? old_bucket->transit : transit).move(bucket->transit, route);
								
								std::cerr << bucket->queues.count(route) << '\n';
							}
						}
						
						bucket->limit = std::stoi(headers["X-RateLimit-Limit"].get<std::string>());
						bucket->remaining = std::min(bucket->remaining, std::stoi(headers["X-RateLimit-Remaining"].get<std::string>()));
						
						bucket->reset.reset();
						bucket->reset = std::make_unique<boost::asio::steady_timer>(*aioc);
						bucket->reset->expires_after(
							std::chrono::seconds(std::stoi(headers["X-RateLimit-Reset-After"].get<std::string>()))
						);
						bucket->reset->async_wait(
							[this, owner = bucket](const boost::system::error_code &e){
								// Don't reset the limit if the timer is cancelled
								if(e.failed()) return;
								log::log(log::trace, [owner](std::ostream *log){
									*log << "Limit reset for " << owner->id << '\n';
								});
								// Reset the limit
								owner->remaining = owner->limit;
								// Kickstart the message sending process
								aioc->post([this]{do_some_work();});
							}
						);
						
						// Run the user's onRead callback
						if(call->onRead != nullptr){
							(*call->onRead)(msg);
						}
					}
				)
			);
		}
		
		struct Call{
			sptr<const std::string> requestType;
			sptr<const std::string> targetURL;
			route_t route;
			sptr<const json> body;
			sptr<const std::function<void()>> onWrite;
			sptr<const std::function<void(const json)>> onRead;
			std::time_t created = std::time(nullptr);
		};
		
		template<typename T>
		struct CountedSet{
			size_t total() const{
				return sum;
			}
			
			size_t count(T t) const{
				return (map.count(t)
				        ? map.at(t)
				        : 0
				);
			}
			
			bool empty() const{
				return !sum;
			}
			
			void insert(T t, size_t count = 1){
				if(!count) return;
				
				auto entry = map.find(t);
				sum += count;
				if(entry != map.end()){
					entry->second += count;
				}else{
					map.emplace(t, count);
				}
			}
			
			void erase(T t, size_t count = 1){
				if(!count) return;
				
				auto entry = map.find(t);
				if(entry == map.end()) throw "Erased a key that doesn't exist";
				if(count > entry->second) throw "Erased a key by more than its value";
				sum -= count;
				entry->second -= count;
				if(entry->second == 0){
					map.erase(t);
				}
			}
			
			size_t clear(T t){
				auto num = count(t);
				map.erase(t);
				return num;
			}
			
			void move(CountedSet<T> &other, T t){
				other.insert(t, clear(t));
			}
			
			void copy(CountedSet<T> &other, T t){
				other.insert(t, count(t));
			}
		
		private:
			size_t sum = 0;
			std::map<T, size_t> map;
		};
		
		struct Bucket{
			std::string id;
			
			QueueByRoute queues;
			CountedSet<route_t> transit;
			
			int limit = 5;
			int remaining = 4;
			std::unique_ptr<boost::asio::steady_timer> reset;
		};
		
		bool writing = false;
		
		// These are for uncategorized calls
		QueueByRoute queues;
		CountedSet<route_t> transit;
		
		std::map<route_t, std::string> route_to_bucket;
		std::map<std::string, Bucket> buckets;
		
		const route_t gateway_route = getLimitedRoute("/gateway/bot");
		route_t getLimitedRoute(const std::string &route){
			std::ostringstream out;
			route_t last = route.find('/');
			route_t next = route.find('/', last + 1);
			std::string lastItem;
			while(last != std::string::npos){
				std::string item = route.substr(last + 1, next - 1);
				out << "|";
				if(std::all_of(item.begin(), item.end(), [](char c){return std::isalpha(c);}) ||
				   lastItem == "channels" || lastItem == "guilds" || lastItem == "webhooks"){
					out << item;
				}
				lastItem = item;
				last = next;
				next = route.find('/', last + 1);
			}
			return std::hash<std::string>{}(out.str());
		}
	};
}