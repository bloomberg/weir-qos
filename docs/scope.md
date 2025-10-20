## Project Goal
Provide an easy-to-configure and easy-to-operate, self-contained middleware for bounding the resource consumption of users of an HTTP API, with the primary goal of protecting the underlying system from load-induced failure.


## Assumptions made during design & implementation
Below we document assumptions made during design and implementation of the system. They should be used as a guide both to tell if the project is suitable for your use-case, but also to guide future technical discussions.
These assumptions may help to answer questions about why particular technical decisions were made.
If one wanted to change one of these assumptions, having them listed also provides some context on the type and quantity of work that such a change would involve.

### Scale assumptions
* Our cluster of front-line web servers has tens of instances
* We serve a number users on the order of thousands, not millions
* Each user makes hundreds to thousands of requests each second
* Each request could transmit an amount of data ranging from a few kilobytes to tens of gigabytes


### Technical assumptions
* HAProxy and the syslog-server that it writes to are running on the same host, communicating via the loopback device (and are therefore generally not subject to dropped UDP packets unless one can't keep up with the other)
* All HAProxy hosts in the cluster have very well-aligned clocks.
    * In particular, the maximum clock drift among hosts in the cluster is much less than one second, preferably within a few milliseconds.
    * This is necessary because different hosts need to agree on timestamps. Usage from each HAProxy instance gets added together based on a timestamp from that instance. Those timestamps need to align reasonably well for this to make sense.


### Operational assumptions
* Our users are not acting maliciously
    * In particular this means we have historically not been overly concerned about users doing things like intentionally sending invalid requests using other users' access keys to starve out those users, or sending huge amounts of data in request headers (which we do not currently count towards bandwidth usage).


## Desired system behaviours
* Bandwidth limits are enforced independently for upload and download traffic
    * In particular, users can max out their configured upload and download limit simultaneously.
    * For small enough limits, this should be possible on a single instance (where small enough means "small enough that it is technically feasible to reach the full configured limit on a single host").
* The system converges to the configured limit “locally” in time, not globally
    * If we find ourselves in a situation where a user has significantly exceeded their limit 5 seconds ago, we will not continue to throttle that user in an attempt to bring them back in line with the configured limit when averaged over all time. We consider the current and previous second, anything older than that is disregarded.
    * More specifically, if a user has for any reason received twice their intended throughput for the past seconds, we’re not going to give them zero throughput for next 10 seconds so that their average across all time is the correct value. Instead we will heavily limit them this second, after which their past excessive usage will be discarded and we’ll go back to just preventing them from exceeding their limit going forward.
* Client's observe a data transfer rate that converges to a stable value
    * In particular clients should not observe a significant oscillation in their rate of data transfer as a result of our limiting
    * Instead the ideal behaviour is for client transfers to quickly converge to a stable throughput that changes only when the number of concurrent requests changes (in which case the same limit needs to be split between a different number of requests and so observed throughput is expected to change).
* Observed error in limit enforcement does not scale with cluster size or attempted usage
    * As a distributed system that reactively enforces limits on usage, we are almost guaranteed to observe some amount of "error", where users are able to exceed their limits some of the time.
    * This error should be temporary and minimised, and in particular should not scale up as the size of the Weir cluster scales up, or as the attempted usage by clients scales up


## Non-goals
* Maintaining 100% strict adherence to limits
    * We accept that users will be able to exceed their configured limits by a relatively small margin temporarily.
    * This should be avoided where possible but making it a strict requirement limits possible solutions to the more computationally-expensive variety and we don't necessarily consider that an acceptable limitation.
    * Conversely the excess should be quantifiable and bounded, and the observed throughput should converge to the configured limit in a reasonable amount of time.
