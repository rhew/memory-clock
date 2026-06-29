FROM python:3.12-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        fonts-lato \
        fonts-noto-core \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

COPY clock_server.py ./

EXPOSE 8000

CMD ["python", "clock_server.py", "--host", "0.0.0.0", "--port", "8000", "--calendar", "/data/calendar.yaml", "--devices", "/data/devices.jsonl"]
