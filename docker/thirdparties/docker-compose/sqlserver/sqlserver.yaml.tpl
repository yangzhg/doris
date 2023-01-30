#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to You under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

version: '3'
services:
  doris--sqlserver_2022:
    image: "mcr.microsoft.com/mssql/server:2022-latest"
    container_name: "doris--sqlserver_2022"
    ports:
      - ${DOCKER_SQLSERVER_EXTERNAL_PORT}:1433
    healthcheck:
      test: ["CMD", "/opt/mssql-tools/bin/sqlcmd", "-Usa", "-PDoris123456", "-Q", "select 1"]
      interval: 5s
      timeout: 30s
      retries: 120
    volumes:
        - ./init:/docker-entrypoint-initdb.d
    restart: always
    environment:
      # Accept the end user license Agreement
      - ACCEPT_EULA=Y
      # password of SA
      - SA_PASSWORD=Doris123456
    networks:
      - doris--sqlserver_2022
  hello-world:
      image: hello-world
      depends_on:
        doris--sqlserver_2022:
          condition: service_healthy

networks:
  doris--sqlserver_2022: