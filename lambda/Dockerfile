FROM public.ecr.aws/lambda/python:3.11

# System libraries required by opencv-python-headless
RUN yum install -y \
    mesa-libGL \
    glib2 \
    libSM \
    libXrender \
    libXext \
    && yum clean all

COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

COPY app.py ./
COPY model11.onnx ./

CMD ["app.lambda_handler"]
