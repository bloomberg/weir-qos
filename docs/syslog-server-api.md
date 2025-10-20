## Syslog server control-plane API

Weir's control loop involves HAProxy sending control messages to our syslog server (as described in [the architecture overview](./architecture_overview.md)).
These messages are sent via UDP, to a socket on which the syslog server is listening.
Upon receipt, those messages are processed depending on their nature, as follows:

1. If they're control messages, they are queued for internal processing
2. If they appear JSON-formatted, they are assumed to be access logs and are written to the access log file
3. Otherwise they are written to the regular/non-access-log log file

Cases 2 and 3 are just different forms of regular logging, but case 1 is where control information is propagated around the system.
After being queued, a separate processing thread will pick up a control message, parse it and store the relevant update, ready to be sent to Redis.

These control messages all follow a similar format: They're all text-based (since they're just log messages from haproxy's regular logging infrastructure), they're all made up of a number of fields, separated by `~|~`, and the first field is always the message type.

The different message types are given below, with an example for each:

### Request Start

```
req~|~1.2.3.4:58840~|~AKIAIOSFODNN7EXAMPLE~|~PUT~|~up~|~instance1234~|~7
```

This is transmitted when HAProxy begins processing a request, just after the headers of the request have been received but before any request body has been transmitted.

The fields are:
1. Message type, constant: `req` indicates this is a "request start" message
2. Request key, string: Uniquely identifies this request among all those currently being processed on this haproxy instance. At the time of writing this field is deprecated and may be removed in future.
3. User key, string: Uniquely identifies the "user" who sent this request. All requests with the same user key will be rate limited together.
4. Verb, string: The class of request limit that this request counts towards and is rate limited for. At the time of writing this is always the HTTP method of the request.
5. Direction, enum: The class of bandwidth limit that this request is rate limited for. Must be either `up` or `dwn` for "upload" and "download" traffic respectively. If a request both uploads and downloads data, only one of those directions will be limited.
6. Instance ID, string: Uniquely identifies the instance of HAProxy that sent this message, and by extension that is processing this request.
7. Active requests, integer: The number of requests that are being actively processed by the sending HAProxy instance for this request's user at the time that this message was sent.

This message serves to count the requests a user is making (for enforcing request limits) as well as indicate an increase in the number of active requests (for enforcing active request limits). It also powers metrics on the number of requests each user is making.

### Request End

```
req_end~|~1.2.3.4:58840~|~AKIAIOSFODNN7EXAMPLE~|~PUT~|~up~|~instance1234~|~7
```

This is transmitted when HAProxy stops processing a request, after the last of the response body has been transmitted.

The fields are the same as those in [Request Start](#request-start), with the exception of the message type which is `req_end` in this case.

This message serves as primarily an update on the number of active requests (for enforcing active request limits).

### Data Transfer

```
data_xfer~|~1.2.3.4:55094~|~AKIAIOSFODNN7EXAMPLE~|~up~|~4096
```

This is transmitted every time HAProxy forwards a chunk of data on to the backend server.

The fields are as follows (with descriptions matching those in [Request Start](#request-start) where none is given):
1. Message type, constant: `data_xfer` indicates this is a "data transfer" message
2. Request key, string
3. User key, string
4. Direction, enum
5. Length, integer: The number of bytes transmitted in this chunk

This message serves to count the amount of data the user is transferring (for enforcing bandwidth limits). It also provides metrics on the amount of data each user is transferring.

### Active Requests

```
active_reqs~|~instanceid-1234~|~AKIAIOSFODNN7EXAMPLE~|~up~|~7
```

Transmitted periodically for every user with active requests being processed on that HAProxy instance.

The fields are as follows (with descriptions matching those in [Request Start](#request-start) where none is given):
1. Message type, constant: `active_reqs` indicates this is an "active requests" message
2. Instance ID, string
3. User key, string
4. Direction, enum
5. Active requests, integer: The number of requests that are actively being processed at the time by this HAProxy instance for this user

This message serves to keep the active request count in Redis up-to-date.
In the event that an HAProxy or syslog-server instance crashes or disappears from the network for any reason, those keys will time-out and be removed from Redis.
To ensure this does not happen erroneously when there are only a few long-running requests in the system for a given user, we send this update to periodically refresh that information.
