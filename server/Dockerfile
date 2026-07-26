FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1
ENV MEMORY_CLOCK_ALERTS_PATH=/data/alerts.yaml

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        fonts-lato \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

COPY clock_server.py ./
COPY alerts.example.yaml ./
COPY admin ./admin

EXPOSE 8000

CMD ["python", "clock_server.py", "--host", "0.0.0.0", "--port", "8000", "--calendar", "/data/calendar.yaml", "--devices", "/data/devices.jsonl", "--state", "/state/memory-clock.sqlite3"]
