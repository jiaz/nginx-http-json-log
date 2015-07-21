# JSON format log for nginx

## Description

nginx by default produces access logs in a flat file in plain text. This is
sometimes inconvenient for log processors to work with.

I saw some people solve the problem by manually construct a json template in
the `log\_format` directive. This is not only inconvenient but also potential
to generate malformed json strings.

So I created this module to mimic the http log module while producing json
format logs when written to file.

## Dependencies

This module require libjansson (>= 2.5.0)

For ubuntu you can run

```
sudo apt-get install libjansson-dev
```

to install the dependency.

## How to build

Assume you clone the source code in `~/workspace/nginx-http-json-log` folder.

1. Get a copy of nginx source code in `~/workspace/nginx`
2. cd to `~/workspace/nginx`
3. Run `./auto/config [other options] --add-module='../nginx-http-json-log'`
4. Run `make`

## How to use

After you build nginx with this module, you can add several directives in the
nginx.conf file to config the json log fields and the log file path.

There are 2 new directives introduced by this module

### access_json_log

The directive is used to specify the log path and the json fields, which is
similar to `access_log` directive in the nginx http log module.

To turn off the json log, you can specify `access_json_log off`.

### json_log_fields

This directive is used to select which variables you want to log in the log
file, which is similar to `log_format` in the nginx http log module.

## Sample config

```
http {
    include       mime.types;
    default_type  application/octet-stream;

    json_log_fields main 'remote_addr'
                         'remote_user'
                         'request'
                         'time_local'
                         'status'
                         'body_bytes_sent'
                         'http_user_agent'
                         'http_x_forwarded_for';

    sendfile        on;
    keepalive_timeout  65;

    server {
        listen       80;
        server_name  localhost;

        access_json_log logs/access_json.log main;

        location / {
            root   html;
            index  index.html index.htm;
        }
    }
}
```

Output in log file:

```
{
  "http_x_forwarded_for": "-",
  "http_user_agent": "ApacheBench/2.3",
  "body_bytes_sent": "612",
  "status": "200",
  "remote_user": "-",
  "remote_addr": "127.0.0.1",
  "time_local": "20/Jul/2015:23:48:14 -0700",
  "request": "GET / HTTP/1.0"
}
```

